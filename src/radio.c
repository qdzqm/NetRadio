/* radio.c - 网络收音机播放引擎
 *
 * 纯 Win32 平台 API + 轻量第三方解码库：
 *   HTTP 下载   : WinINet（含 ICY 元数据）
 *   MP3 解码    : minimp3（单文件库）
 *   AAC 解码    : FAAD2（libfaad）
 *   m3u8 直播   : HLS 分片 + MPEG-TS 解封装（hls.c）
 *   PCM 输出    : waveOut
 *
 * 工作线程  网络读 → 格式探测 → 解码 → waveOutWrite
 * 所有对外 API 线程安全，UI 通过 radio_get_info 拉快照。
 */

#define _WIN32_WINNT   0x0600
#define WINVER         0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <mmsystem.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "neaacdec.h"
#include "hls.h"
#include "radio.h"

#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "winmm.lib")
#endif

/* ---------------- 常量 ---------------- */
#define NET_BUF_CAP    (1024 * 1024) /* 压缩数据环形缓冲（约 1 分钟音频） */
#define DEC_BUF_CAP    (128 * 1024)  /* 解码工作缓冲 */
#define PCM_PUSH_CAP   (64 * 1024)   /* 单次解码输出上限 */
#define WO_HDR_COUNT   8
#define WO_HDR_BYTES   (32 * 1024)
#define TARGET_BUF_MS  3500          /* 界面缓冲显示上限（毫秒） */
#define PLAY_MS        4000          /* 开播前预缓冲时长 */
#define REBUF_MS       3000          /* 欠载后重新蓄水量 */
#define HLS_SEG_MAX    (1024 * 1024) /* 单个分片最大 1MB */
#define HLS_ES_CAP     (512 * 1024)  /* 单分片 ES 缓冲 */

/* 编解码类型 */
#define CODEC_NONE 0
#define CODEC_MP3  1
#define CODEC_AAC  2

/* 预缓存会话（定义见文件后部） */
typedef struct PrewarmTag Prewarm;
static void prewarmFreeAll(Prewarm* pw);

/* ---------------- 全局 ---------------- */
static CRITICAL_SECTION   g_lock;
static BOOL               g_lockInited = FALSE;

static HANDLE             g_thread = NULL;
static volatile LONG      g_threadRun = 0;
static volatile LONG      g_quit    = 0;
static volatile LONG      g_pause   = 0;
static volatile LONG      g_active  = 0;

static HWAVEOUT           g_hWo     = NULL;
static HANDLE             g_hWoEvt  = NULL;
static HANDLE             g_hWake   = NULL;   /* 手工唤醒 worker */
static HINTERNET          g_hInet   = NULL;
static HINTERNET          g_hConn   = NULL;
static HINTERNET          g_hReq    = NULL;

static mp3dec_t           g_mp3;               /* minimp3 状态 */
static NeAACDecHandle     g_faad    = NULL;    /* FAAD2 状态 */
static BOOL               g_faadInited = FALSE;
static WAVEFORMATEX       g_pcmFmt;            /* 当前输出格式 */
static BOOL               g_pcmFmtValid = FALSE;

static DWORD              g_volPct  = 70;
static RADIO_INFO         g_info;

static HANDLE             g_netThread = NULL;   /* 网络线程（流式读取或 HLS 下载） */
static volatile LONG      g_netErr  = 0;        /* 网络彻底失败 */
static volatile LONG      g_netDone = 0;        /* 数据源正常结束 */

/* 网络线程会话上下文：网络线程只通过这里的指针访问环形缓冲与 WinINet 句柄，
 * 不直接碰全局 g_rb/g_rbEvt。切台时 worker 递增代际后，旧网络线程的 gen 与
 * 全局代际不符，便立即停止写入并退出，绝不访问新会话资源，杜绝闪退。 */
typedef struct {
    BYTE*           rb;       /* 本会话环形缓冲 */
    HANDLE          rbEvt;    /* 本会话事件 */
    LONG            gen;      /* 本会话代际 */
    HINTERNET       hInet;    /* 本会话 Internet 根句柄（网络线程关闭） */
    HINTERNET       hConn;    /* 本会话连接句柄（网络线程关闭） */
    HINTERNET       hReq;     /* 本会话流式请求句柄（可被外部强制关闭） */
    volatile HINTERNET killReq; /* HLS 下载请求句柄登记槽，供外部强制中断 */
} NetSess;

/* 会话代际：每次 play/stop 递增。worker 与网络线程启动时捕获自己的代际，
 * 只有“仍是当前代”的线程才允许读写全局共享资源。 */
static volatile LONG      g_gen = 0;
static volatile LONG      g_netGen = 0;   /* 当前网络会话代际（与 g_gen 同步） */
/* 当前会话的网络线程上下文（worker 启动网络线程前设置、join 后置空），
 * 用于强制中断时关闭 HLS/流式阻塞句柄。 */
static NetSess*           g_killSess = NULL;
static volatile LONG      g_bps     = 12000;    /* 估算码率（字节/秒） */
static int                g_fmtKbps = 0;        /* AAC 已显示码率（kbps），估算值变化才刷新 */
static char               g_urlA[1024];         /* 当前播放地址（重连用） */

/* 压缩数据环形缓冲：网络线程写、解码线程读 */
static BYTE*              g_rb    = NULL;
static volatile LONGLONG  g_rbW   = 0;          /* 累计写入字节（单调） */
static volatile LONGLONG  g_rbR   = 0;          /* 累计读出字节（单调） */
static HANDLE             g_rbEvt = NULL;

static void lock(void)   { if (g_lockInited) EnterCriticalSection(&g_lock); }
static void unlock(void) { if (g_lockInited) LeaveCriticalSection(&g_lock); }

static void copyW(WCHAR* dst, int cap, const WCHAR* src) {
    int i = 0;
    for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}
static void setStatusW(const WCHAR* s) { lock(); copyW(g_info.status, 160, s); unlock(); }
static void setState(RADIO_STATE st)   { lock(); g_info.state = st; unlock(); }

/* mlang.dll 编码自动检测（动态加载，避免硬依赖） */
typedef struct { ULONG nLang; UINT nCodePage; } DetectEncodingInfo;
typedef HRESULT (WINAPI *pfnDetectInputCodepage)(DWORD, DWORD, char*, LONG,
                                                 DetectEncodingInfo*, int*);
static UINT detectCodePage(const char* a, int len) {
    static pfnDetectInputCodepage pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("mlang.dll");
        if (m) pfn = (pfnDetectInputCodepage)GetProcAddress(m, "DetectInputCodepage");
    }
    if (!pfn) return 0;
    DetectEncodingInfo info[4]; int nInfo = 4;
    if (pfn(0, 0, (char*)a, len, info, &nInfo) == 0 && nInfo > 0 && info[0].nCodePage)
        return info[0].nCodePage;
    return 0;
}

/* 去掉尾部不完整的 UTF-8 多字节字符（源端可能按长度截断标题） */
static int trimUtf8Tail(char* raw, int m) {
    if (m > 0 && (raw[m - 1] & 0x80)) {
        int i = m - 1;
        while (i > 0 && (raw[i] & 0xC0) == 0x80) i--;   /* 回退到首字节 */
        unsigned char c = (unsigned char)raw[i];
        int need = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 :
                   (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + need > m) { m = i; raw[m] = 0; }
    }
    return m;
}

/* 反转“双重编码 UTF-8”：源端把 UTF-8 字节当 Latin-1 再编成 UTF-8，
 * 解码后得到一串 å¾ 式拉丁字符。特征：高位字符全部落在 U+0080–U+00FF。
 * 把字符压回单字节再按 UTF-8 解一次即还原；不像双重编码则返回 0。 */
static int undoDoubleUtf8(const WCHAR* w, int wn, WCHAR* out, int outCap) {
    int lat = 0, tot = 0;
    for (int i = 0; i < wn; i++) {
        if (w[i] >= 0x80) { tot++; if (w[i] <= 0xFF) lat++; }
    }
    if (tot == 0 || lat != tot) return 0;
    char raw[256]; int m = 0;
    for (int i = 0; i < wn && m < (int)sizeof(raw) - 1; i++) raw[m++] = (char)w[i];
    raw[m] = 0;
    m = trimUtf8Tail(raw, m);
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, -1, out, outCap);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) if (out[i] >= 0x80) return n;  /* 还原后须含多字节字符 */
    return 0;   /* 纯 Latin-1 标题（如 Café）不反转 */
}

static void setIcyTitleA(const char* a) {
    WCHAR buf[256];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a, -1, buf, 250);
    if (n > 0) {
        WCHAR fixed[256];
        int fn = undoDoubleUtf8(buf, n - 1, fixed, 250);
        if (fn > 0) { fixed[fn] = 0; memcpy(buf, fixed, (fn + 1) * sizeof(WCHAR)); n = fn + 1; }
    }
    if (n <= 0) {
        /* 非 UTF-8：先去掉源端截断的尾部残字重试，
         * 仍失败再用 mlang 探测（GBK/Big5 等），最后回退系统代码页 */
        char raw[256];
        int m = (int)strlen(a); if (m > 255) m = 255;
        memcpy(raw, a, m); raw[m] = 0;
        m = trimUtf8Tail(raw, m);
        n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, -1, buf, 250);
        if (n <= 0) {
            UINT cp = detectCodePage(raw, m);
            if (cp) n = MultiByteToWideChar(cp, 0, raw, -1, buf, 250);
            if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, raw, -1, buf, 250);
        }
    }
    if (n <= 0) buf[0] = 0; else buf[n - 1] = 0;
    lock(); copyW(g_info.icyTitle, 256, buf); unlock();
}
static void setFmtInfo(const WCHAR* codec, int kbps, int sr, int ch) {
    WCHAR b[64];
    const WCHAR* srStr = L"?";
    switch (sr) {
        case 44100: srStr = L"44.1"; break;
        case 48000: srStr = L"48";   break;
        case 32000: srStr = L"32";   break;
        case 22050: srStr = L"22.05";break;
        case 24000: srStr = L"24";   break;
        case 16000: srStr = L"16";   break;
        case 12000: srStr = L"12";   break;
        case 11025: srStr = L"11.025";break;
        case 8000:  srStr = L"8";    break;
    }
    if (kbps > 0)
        _snwprintf(b, 63, L"%s %d kbps  %s kHz  %s",
                   codec, kbps, srStr, ch == 1 ? L"Mono" : L"Stereo");
    else
        _snwprintf(b, 63, L"%s %s kHz  %s",
                   codec, srStr, ch == 1 ? L"Mono" : L"Stereo");
    b[63] = 0;
    lock(); copyW(g_info.fmtInfo, 64, b); unlock();
}
static void setBuf(int curMs) { lock(); g_info.bufCur = curMs; g_info.bufMax = TARGET_BUF_MS; unlock(); }

static void resetInfo(const WCHAR* urlW) {
    lock();
    memset(&g_info, 0, sizeof(g_info));
    g_info.state  = RS_IDLE;
    g_info.bufMax = TARGET_BUF_MS;
    if (urlW) copyW(g_info.url, 512, urlW);
    copyW(g_info.status, 160, L"空闲");
    unlock();
}

static BOOL quitNow(void) { return InterlockedCompareExchange(&g_quit, 0, 0) != 0; }

/* ---------------- 压缩数据环形缓冲 ----------------
 * 网络线程生产、解码线程消费，把网络抖动与播放彻底解耦。
 * 单生产者单消费者，用 64 位单调计数定位（x86 对齐写入原子）。 */
static int  ringFill(void) { return (int)(g_rbW - g_rbR); }
static void ringReset(void) { g_rbW = 0; g_rbR = 0; }

/* 网络线程写环：会话失效（切台/退出）立即停止，避免写已释放内存。
 * 返回写入字节数；-1 表示会话已失效，调用方应立刻结束线程。 */
static int sessRingWrite(NetSess* s, const BYTE* p, int n) {
    int off = 0;
    while (off < n) {
        if (quitNow() || s->gen != g_netGen || !s->rb) return off > 0 ? off : -1;
        LONGLONG w = g_rbW;
        int space = NET_BUF_CAP - (int)(w - g_rbR);
        if (space <= 0) {
            /* 缓冲满：直播丢旧数据贴近直播边缘，而不是无限等待 */
            LONGLONG drop = NET_BUF_CAP / 4;
            g_rbR = w - NET_BUF_CAP + drop;
            WaitForSingleObject(s->rbEvt, 30);
            continue;
        }
        int k = n - off; if (k > space) k = space;
        int pos = (int)(w % NET_BUF_CAP);
        int part = NET_BUF_CAP - pos; if (k > part) k = part;
        memcpy(s->rb + pos, p + off, k);
        g_rbW = w + k;
        off += k;
        SetEvent(s->rbEvt);
    }
    return off;
}

/* 写入 n 字节，阻塞直到写完或退出；返回实际写入量（解码线程用，本会话内安全） */
static int ringWrite(const BYTE* p, int n) {
    int off = 0;
    while (off < n && !quitNow()) {
        LONGLONG w = g_rbW;
        int space = NET_BUF_CAP - (int)(w - g_rbR);
        if (space <= 0) { WaitForSingleObject(g_rbEvt, 100); continue; }
        int k = n - off; if (k > space) k = space;
        int pos = (int)(w % NET_BUF_CAP);
        int part = NET_BUF_CAP - pos; if (k > part) k = part;
        memcpy(g_rb + pos, p + off, k);
        g_rbW = w + k;
        off += k;
        SetEvent(g_rbEvt);
    }
    return off;
}

/* 非阻塞读取至多 want 字节 */
static int ringRead(BYTE* dst, int want) {
    int got = 0;
    while (got < want) {
        LONGLONG r = g_rbR;
        int fill = (int)(g_rbW - r);
        if (fill <= 0) break;
        int k = want - got; if (k > fill) k = fill;
        int pos = (int)(r % NET_BUF_CAP);
        int part = NET_BUF_CAP - pos; if (k > part) k = part;
        memcpy(dst + got, g_rb + pos, k);
        g_rbR = r + k;
        got += k;
        SetEvent(g_rbEvt);
    }
    return got;
}

/* 丢弃最旧的 n 字节（直播缓冲过深时保持贴近直播边缘） */
static void ringSkip(int n) {
    int fill = ringFill();
    if (n > fill) n = fill;
    if (n > 0) { g_rbR += n; SetEvent(g_rbEvt); }
}

/* 等待缓冲达到 bytes 字节（退出/网络终止也会返回，由调用方判断） */
static void ringWaitBytes(int bytes) {
    while (!quitNow() && !g_netErr && !g_netDone && ringFill() < bytes)
        WaitForSingleObject(g_rbEvt, 100);
}

/* 可被退出打断的延时 */
static void quitSleep(int ms) {
    for (int t = 0; t < ms && !quitNow(); t += 100) Sleep(100);
}

/* ---------------- waveOut ---------------- */
typedef struct {
    WAVEHDR hdr;
    BYTE*   data;
    BOOL    inUse;
} WoBuf;
static WoBuf g_wo[WO_HDR_COUNT];

/* 暂存缓冲：把小块 PCM 攒满整块再提交。每块 32KB≈170ms，8 块队列约可缓冲 1.4 秒，
 * 网络读取的抖动不会再直接变成音频断点（切台/换格式时须清零） */
static BYTE g_stage[WO_HDR_BYTES];
static int  g_stageLen = 0;

static BOOL woOpen(const WAVEFORMATEX* pcm) {
    MMRESULT r = waveOutOpen(&g_hWo, WAVE_MAPPER, (WAVEFORMATEX*)pcm,
                             (DWORD_PTR)g_hWoEvt, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { g_hWo = NULL; return FALSE; }
    for (int i = 0; i < WO_HDR_COUNT; i++) {
        g_wo[i].data = (BYTE*)VirtualAlloc(NULL, WO_HDR_BYTES,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_wo[i].data) return FALSE;
        memset(&g_wo[i].hdr, 0, sizeof(g_wo[i].hdr));
        g_wo[i].hdr.lpData         = (LPSTR)g_wo[i].data;
        g_wo[i].hdr.dwBufferLength = WO_HDR_BYTES;
        g_wo[i].inUse = FALSE;
        waveOutPrepareHeader(g_hWo, &g_wo[i].hdr, sizeof(WAVEHDR));
    }
    radio_set_volume((int)g_volPct);
    return TRUE;
}
static void woClose(void) {
    if (!g_hWo) return;
    waveOutReset(g_hWo);
    g_stageLen = 0;   /* 未提交的暂存数据随播放会话丢弃 */
    for (int i = 0; i < WO_HDR_COUNT; i++) {
        if (g_wo[i].data) {
            waveOutUnprepareHeader(g_hWo, &g_wo[i].hdr, sizeof(WAVEHDR));
            VirtualFree(g_wo[i].data, 0, MEM_RELEASE);
            g_wo[i].data = NULL;
        }
    }
    waveOutClose(g_hWo);
    g_hWo = NULL;
}
static WoBuf* woGetFree(void) {
    for (int i = 0; i < WO_HDR_COUNT; i++) if (!g_wo[i].inUse) return &g_wo[i];
    return NULL;
}
static void woReclaim(void) {
    for (int i = 0; i < WO_HDR_COUNT; i++)
        if (g_wo[i].inUse && (g_wo[i].hdr.dwFlags & WHDR_DONE)) g_wo[i].inUse = FALSE;
}
static int woPendingBytes(void) {
    int n = 0;
    for (int i = 0; i < WO_HDR_COUNT; i++)
        if (g_wo[i].inUse) n += (int)g_wo[i].hdr.dwBufferLength;
    return n;
}

/* 确保音频设备按指定格式打开（格式变化时自动重开）。返回 0 成功 */
static int ensureAudio(int sr, int ch) {
    if (g_pcmFmtValid && g_hWo &&
        g_pcmFmt.nSamplesPerSec == (DWORD)sr && g_pcmFmt.nChannels == (WORD)ch)
        return 0;
    if (g_hWo) woClose();
    memset(&g_pcmFmt, 0, sizeof(g_pcmFmt));
    g_pcmFmt.wFormatTag      = WAVE_FORMAT_PCM;
    g_pcmFmt.nChannels       = (WORD)ch;
    g_pcmFmt.nSamplesPerSec  = (DWORD)sr;
    g_pcmFmt.wBitsPerSample  = 16;
    g_pcmFmt.nBlockAlign     = (WORD)(ch * 2);
    g_pcmFmt.nAvgBytesPerSec = (DWORD)sr * ch * 2;
    if (!woOpen(&g_pcmFmt)) return -1;
    g_pcmFmtValid = TRUE;
    return 0;
}

/* 提交一整块到声卡，阻塞直到写入或退出。返回 0 成功 */
static int woSubmit(const BYTE* data, int len) {
    while (!quitNow()) {
        WoBuf* wb = woGetFree();
        if (!wb) { WaitForSingleObject(g_hWoEvt, 60); woReclaim(); continue; }
        memcpy(wb->data, data, len);
        wb->hdr.dwFlags &= WHDR_PREPARED;
        wb->hdr.dwLoops = 0;
        wb->hdr.dwBytesRecorded = 0;
        wb->hdr.dwBufferLength = (DWORD)len;
        wb->inUse = TRUE;
        MMRESULT wr = waveOutWrite(g_hWo, &wb->hdr, sizeof(WAVEHDR));
        if (wr != MMSYSERR_NOERROR) {
            wb->inUse = FALSE;
            return -1;
        }
        return 0;
    }
    return -1;
}

/* 把 PCM 推入音频设备（先攒入暂存缓冲，满块才提交）。返回 0 成功 */
static int pushPcm(const BYTE* pcm, int bytes) {
    int align = g_pcmFmtValid ? (int)g_pcmFmt.nBlockAlign : 4;
    if (align <= 0) align = 4;
    bytes = (bytes / align) * align;

    int off = 0;
    while (off < bytes) {
        if (quitNow()) return -1;
        int n = bytes - off;
        if (n > WO_HDR_BYTES - g_stageLen) n = WO_HDR_BYTES - g_stageLen;
        memcpy(g_stage + g_stageLen, pcm + off, n);
        g_stageLen += n;
        off += n;
        if (g_stageLen < WO_HDR_BYTES) break;   /* 未攒满，等后续数据 */
        if (woSubmit(g_stage, g_stageLen) != 0) return -1;
        g_stageLen = 0;
    }
    return 0;
}

/* 暂停等待 + 缓冲进度刷新（主循环公共逻辑）。返回非 0 表示应退出 */
static int loopHousekeeping(void) {
    while (InterlockedCompareExchange(&g_pause, 0, 0) && !quitNow()) {
        WaitForSingleObject(g_hWake, 100);
        woReclaim();
    }
    if (quitNow()) return 1;
    woReclaim();
    /* 缓冲显示：环形缓冲折算毫秒 + 声卡队列毫秒 */
    long bps = g_bps > 1000 ? g_bps : 12000;
    int ms = (int)((ULONGLONG)ringFill() * 1000 / (ULONGLONG)bps);
    if (g_pcmFmtValid && g_pcmFmt.nAvgBytesPerSec)
        ms += (int)((ULONGLONG)woPendingBytes() * 1000 / g_pcmFmt.nAvgBytesPerSec);
    setBuf(ms);
    return 0;
}

/* ---------------- MP3 帧头解析（仅用于格式探测与显示） ---------------- */
static const int brM1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const int brM2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
static const int srTab[3] = {44100, 48000, 32000};

static int tryParseMp3(const unsigned char* p, int avail, int* frameSize) {
    if (avail < 4) return 0;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int verBits = (p[1] >> 3) & 3;
    int layBits = (p[1] >> 1) & 3;
    if (layBits != 1) return 0;
    int ver = (verBits == 3) ? 1 : (verBits == 2) ? 2 : (verBits == 0) ? 3 : 0;
    if (!ver) return 0;
    int brIdx = (p[2] >> 4) & 0xF;
    int srIdx = (p[2] >> 2) & 0x3;
    int pad   = (p[2] >> 1) & 0x1;
    if (brIdx == 0 || brIdx == 15 || srIdx == 3) return 0;
    int kbps = (ver == 1) ? brM1[brIdx] : brM2[brIdx];
    if (!kbps) return 0;
    int sr = srTab[srIdx];
    if (ver == 2) sr /= 2;
    if (ver == 3) sr /= 4;
    int fs = (ver == 1) ? (144000 * kbps / sr + pad) : (72000 * kbps / sr + pad);
    if (fs < 24 || fs > 2000) return 0;
    if (frameSize) *frameSize = fs;
    return 1;
}

/* ADTS 帧头探测：返回 1 合法并输出帧长 */
static int tryParseAdts(const unsigned char* p, int avail, int* frameSize) {
    if (avail < 7) return 0;
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0) return 0;
    int fs = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);
    if (fs < 8 || fs > 8192) return 0;
    if (frameSize) *frameSize = fs;
    return 1;
}

static int id3v2Size(const unsigned char* p, int avail) {
    if (avail < 10) return 0;
    if (memcmp(p, "ID3", 3) != 0) return 0;
    int sz = ((p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14)
           | ((p[8] & 0x7F) <<  7) |  (p[9] & 0x7F);
    return 10 + sz;
}

/* 在缓冲中探测首个可靠的 MP3/ADTS 帧，返回 CODEC_*；*off 为帧偏移 */
static int probeCodec(const BYTE* buf, int len, int* off) {
    int i, mp3Pos = -1, aacPos = -1;
    for (i = 0; i + 4 < len && mp3Pos < 0; i++) {
        int fs;
        if (tryParseMp3(buf + i, len - i, &fs)) {
            /* 交叉验证下一帧 */
            if (i + fs + 4 <= len) {
                if (tryParseMp3(buf + i + fs, len - i - fs, NULL)) mp3Pos = i;
            } else mp3Pos = i;
        }
    }
    for (i = 0; i + 7 < len && aacPos < 0; i++) {
        int fs;
        if (tryParseAdts(buf + i, len - i, &fs)) {
            if (i + fs + 7 <= len) {
                if (tryParseAdts(buf + i + fs, len - i - fs, NULL)) aacPos = i;
            } else aacPos = i;
        }
    }
    if (mp3Pos < 0 && aacPos < 0) return CODEC_NONE;
    if (mp3Pos >= 0 && (aacPos < 0 || mp3Pos <= aacPos)) { *off = mp3Pos; return CODEC_MP3; }
    *off = aacPos;
    return CODEC_AAC;
}

/* ---------------- HTTP + ICY 封装（流式） ---------------- */
typedef struct {
    int metaint;
    int audioLeft;
    int metaRemain;
    char meta[512];
    int  metaLen;
    int  silent;   /* 预缓存用：解析到元数据不更新界面 */
} IcyCtx;

/* 预缓存会话：提前为目标台建连并缓冲数据，切台时被 worker 接管 */
#define PW_BUF_CAP   (512 * 1024)   /* 流式预缓存缓冲上限 */
#define PW_READY_MIN (32 * 1024)    /* 缓冲达到此量后认为可接管 */

struct PrewarmTag {
    char urlA[1024];
    volatile LONG quit;    /* 请求终止（切台/停止） */
    volatile LONG ready;   /* 已拿到足够数据，可被接管 */
    volatile LONG claim;   /* 已被认领：线程退出时不得自清理 */
    int isHls;
    HINTERNET hInet, hConn, hReq;
    IcyCtx icy;
    char ct[128];          /* 流式响应的 Content-Type */
    BYTE* buf; int bufLen;
    char plUrl[1024];      /* HLS：已解析到的媒体列表 URL */
    BYTE* plBuf; int plLen;
    char segUrl[768];      /* HLS：缓存的最后一片 */
    BYTE* segBuf; int segLen;
    HANDLE thread;
};

static BOOL httpOpenEx(HINTERNET* oInet, HINTERNET* oConn, HINTERNET* oReq,
                       const char* urlA, IcyCtx* icy, char* ctOut, int ctCap,
                       DWORD recvTimeoutMs, BOOL silent) {
    URL_COMPONENTSA uc;
    char hostA[256] = {0}, pathA[512] = {0}, extraA[256] = {0};
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = hostA;  uc.dwHostNameLength  = sizeof(hostA) - 1;
    uc.lpszUrlPath      = pathA;  uc.dwUrlPathLength   = sizeof(pathA) - 1;
    uc.lpszExtraInfo    = extraA; uc.dwExtraInfoLength = sizeof(extraA) - 1;
    if (!InternetCrackUrlA(urlA, 0, 0, &uc)) return FALSE;

    /* DIRECT：不走系统代理自动检测（WPAD），避免每次连接空转数秒 */
    *oInet = InternetOpenA("NetRadio/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!*oInet) return FALSE;

    *oConn = InternetConnectA(*oInet, hostA, uc.nPort, NULL, NULL,
                               INTERNET_SERVICE_HTTP, 0, 0);
    if (!*oConn) return FALSE;

    char full[768];
    if (extraA[0]) _snprintf(full, sizeof(full) - 1, "%s%s", pathA, extraA);
    else           _snprintf(full, sizeof(full) - 1, "%s", pathA);
    full[sizeof(full) - 1] = 0;

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_PRAGMA_NOCACHE;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS)
        flags |= INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID
                             | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;

    *oReq = HttpOpenRequestA(*oConn, "GET", full, "HTTP/1.1", NULL, NULL, flags, 0);
    if (!*oReq) return FALSE;
    {   /* 超时保护：避免切台时阻塞在读取上无法退出 */
        DWORD ms = 8000;
        InternetSetOptionA(*oReq, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
        InternetSetOptionA(*oReq, INTERNET_OPTION_SEND_TIMEOUT,    &ms, sizeof(ms));
        DWORD rt = recvTimeoutMs ? recvTimeoutMs : 8000;
        InternetSetOptionA(*oReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &rt, sizeof(rt));
    }

    static const char* hdrs =
        "Icy-MetaData: 1\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n";
    if (!HttpSendRequestA(*oReq, hdrs, (DWORD)strlen(hdrs), NULL, 0)) return FALSE;

    char status[32]; DWORD szl = sizeof(status);
    if (HttpQueryInfoA(*oReq, HTTP_QUERY_STATUS_CODE, status, &szl, NULL)) {
        int code = atoi(status);
        if (code < 200 || code >= 300) {
            if (!silent) {
                WCHAR msg[64];
                _snwprintf(msg, 63, L"HTTP 错误 %d", code);
                setStatusW(msg);
            }
            return FALSE;
        }
    }
    {
        char qbuf[64] = "icy-metaint";
        DWORD qbs = sizeof(qbuf);
        if (HttpQueryInfoA(*oReq, HTTP_QUERY_CUSTOM, qbuf, &qbs, NULL)) {
            icy->metaint = atoi(qbuf);
        } else icy->metaint = 0;
    }
    icy->audioLeft = icy->metaint;
    icy->metaRemain = 0;
    icy->metaLen = 0;

    if (ctOut && ctCap > 0) {
        ctOut[0] = 0;
        DWORD cts = (DWORD)ctCap;
        HttpQueryInfoA(*oReq, HTTP_QUERY_CONTENT_TYPE, ctOut, &cts, NULL);
    }
    return TRUE;
}

static void icyParseMeta(IcyCtx* icy) {
    int nullPos = icy->metaLen;
    if (nullPos > (int)sizeof(icy->meta) - 1) nullPos = (int)sizeof(icy->meta) - 1;
    icy->meta[nullPos] = 0;
    if (icy->silent) { icy->metaLen = 0; return; }   /* 预缓存：不碰界面 */
    const char* p = strstr(icy->meta, "StreamTitle='");
    if (p) {
        p += strlen("StreamTitle='");
        const char* q = strchr(p, '\'');
        char tmp[256] = {0};
        int len = q ? (int)(q - p) : (int)strlen(p);
        if (len > 240) len = 240;
        memcpy(tmp, p, len); tmp[len] = 0;
        setIcyTitleA(tmp);
    }
    icy->metaLen = 0;
}

/* 读 want 字节音频到 dst，返回实际读到的字节数；0=EOF, -1=错误 */
static int icyReadEx(HINTERNET hReq, IcyCtx* icy, volatile LONG* quit,
                     BYTE* dst, int want) {
    int done = 0;
    while (done < want) {
        if (quit && *quit) return done ? done : -1;
        if (icy->metaRemain > 0) {
            DWORD g = 0;
            char tmp[256];
            int piece = icy->metaRemain; if (piece > 240) piece = 240;
            if (!InternetReadFile(hReq, tmp, piece, &g) || g == 0) return done ? done : -1;
            if (icy->metaLen + (int)g < (int)sizeof(icy->meta)) {
                memcpy(icy->meta + icy->metaLen, tmp, g);
                icy->metaLen += (int)g;
            }
            icy->metaRemain -= (int)g;
            if (icy->metaRemain == 0) icyParseMeta(icy);
            continue;
        }
        int budget = want - done;
        if (icy->metaint > 0) {
            if (icy->audioLeft <= 0) icy->audioLeft = icy->metaint;
            if (budget > icy->audioLeft) budget = icy->audioLeft;
        }
        DWORD g = 0;
        if (!InternetReadFile(hReq, dst + done, budget, &g)) return done ? done : -1;
        if (g == 0) return done ? done : 0;
        done += (int)g;
        if (icy->metaint > 0) {
            icy->audioLeft -= (int)g;
            if (icy->audioLeft == 0) {
                BYTE b = 0; DWORD gg = 0;
                if (!InternetReadFile(hReq, &b, 1, &gg) || gg == 0) return done;
                icy->metaRemain = (int)b * 16;
            }
        }
    }
    return done;
}

/* ---------------- 解码器生命周期 ---------------- */
static void decodersReset(void) {
    mp3dec_init(&g_mp3);
    if (g_faad) { NeAACDecClose(g_faad); g_faad = NULL; }
    g_faadInited = FALSE;
    g_pcmFmtValid = FALSE;
    g_fmtKbps = 0;
}
static BOOL faadPrepare(void) {
    g_faad = NeAACDecOpen();
    if (!g_faad) return FALSE;
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(g_faad);
    if (cfg) {
        cfg->outputFormat = FAAD_FMT_16BIT;
        cfg->downMatrix   = 0;   /* 5.1 转 2.0 降混；注意 FAAD2 的 16bit/float 输出对
                                    “原生立体声+downMatrix=1” 会落到未初始化的 5.1
                                    降混分支产生噪声，必须保持 0 */
        NeAACDecSetConfiguration(g_faad, cfg);
    }
    return TRUE;
}

/* ---------------- 缓冲阈值 ---------------- */
static void updateBps(long inst) {
    if (inst < 500) return;
    g_bps = (LONG)(((LONGLONG)g_bps * 7 + inst) / 8);
}
static int thrBytes(int ms) {
    LONGLONG b = (LONGLONG)g_bps * ms / 1000;
    if (b < 32 * 1024) b = 32 * 1024;
    if (b > 512 * 1024) b = 512 * 1024;
    return (int)b;
}

/* 按当前状态恢复状态行文字（重连成功后用） */
static void statusByState(void) {
    lock();
    const WCHAR* s = L"播放中";
    if (g_info.state == RS_BUFFERING) s = L"正在缓冲…";
    else if (g_info.state == RS_PAUSED) s = L"已暂停";
    copyW(g_info.status, 160, s);
    unlock();
}

/* ---------------- 网络线程（流式） ----------------
 * 只管把压缩数据灌入环形缓冲；读错自动重连（指数退避，最多 3 次），
 * 期间解码线程靠存量缓冲继续播，网络抖动不会被耳朵听到。
 * 句柄与会话状态由 NetSess 持有（堆分配，线程退出时自行释放），
 * worker 强制中断时 InternetCloseHandle(s->hReq) 即可解除阻塞。 */
static unsigned __stdcall netStreamThread(void* arg) {
    NetSess* s = (NetSess*)arg;
    IcyCtx icy; memset(&icy, 0, sizeof(icy));
    HINTERNET hInet = s->hInet, hConn = s->hConn, hReq = s->hReq;
    s->hInet = s->hConn = s->hReq = NULL;   /* 接管预连接句柄 */
    BOOL open = (hReq != NULL);
    int retries = 0;
    BYTE tmp[8192];
    while (!quitNow() && s->gen == g_netGen) {
        if (!open) {
            char ct[128] = {0};
            memset(&icy, 0, sizeof(icy));
            if (!httpOpenEx(&hInet, &hConn, &hReq, g_urlA, &icy,
                            ct, sizeof(ct), 8000, FALSE)) {
                if (++retries > 3) { InterlockedExchange(&g_netErr, 1); break; }
                setStatusW(L"连接中断，正在重连…");
                quitSleep(500 << (retries - 1));
                continue;
            }
            open = TRUE; retries = 0;
            statusByState();
        }
        int got = icyReadEx(hReq, &icy, &g_quit, tmp, sizeof(tmp));
        if (got > 0) {
            if (sessRingWrite(s, tmp, got) < 0) break;   /* 会话失效，立即结束 */
            continue;
        }
        if (quitNow() || s->gen != g_netGen) break;
        if (got == 0) { InterlockedExchange(&g_netDone, 1); break; }
        /* 读取错误：断开重连，环形缓冲继续供播 */
        if (hReq) { InternetCloseHandle(hReq); hReq = NULL; }
        if (hConn) { InternetCloseHandle(hConn); hConn = NULL; }
        if (hInet) { InternetCloseHandle(hInet); hInet = NULL; }
        open = FALSE;
        if (++retries > 3) { InterlockedExchange(&g_netErr, 1); break; }
        setStatusW(L"网络中断，正在重连…");
        quitSleep(500 << (retries - 1));
    }
    if (hReq)  InternetCloseHandle(hReq);
    if (hConn) InternetCloseHandle(hConn);
    if (hInet) InternetCloseHandle(hInet);
    /* 不 free(s)：sess 归 worker 所有，由 worker 在 join 本线程后统一释放，
     * 避免 worker 强制中断时访问到已被本线程 free 的 sess（悬垂指针竞态）。 */
    return 0;
}

/* ---------------- 解码无进展处理 ----------------
 * 环形缓冲还有数据：等一会儿；空了：网络终止→结束/报错，
 * 否则判定欠载，回"正在缓冲"重新蓄水后继续（宁可短暂停顿不断续） */
static int starveHandle(void) {
    if (ringFill() > 0) { WaitForSingleObject(g_rbEvt, 10); return 0; }
    if (quitNow()) return 1;
    if (g_netErr)  { setStatusW(L"网络中断"); setState(RS_ERROR); return 1; }
    if (g_netDone) { setStatusW(L"播放结束"); setState(RS_STOPPED); return 1; }
    setState(RS_BUFFERING);
    setStatusW(L"正在缓冲…");
    ringWaitBytes(thrBytes(REBUF_MS));
    if (quitNow()) return 1;
    if (g_netErr)  { setStatusW(L"网络中断"); setState(RS_ERROR); return 1; }
    if (g_netDone) { setStatusW(L"播放结束"); setState(RS_STOPPED); return 1; }
    setState(RS_PLAYING);
    setStatusW(L"播放中");
    return 0;
}

/* ---------------- 统一解码主循环（流式与 HLS 共用） ----------------
 * 开播前攒够 PLAY_MS 的数据再出声；播放中欠载则回缓冲状态重蓄。
 * 网络抖动全部由环形缓冲吸收。 */
static void runDecode(void) {
    BYTE* decBuf = (BYTE*)VirtualAlloc(NULL, DEC_BUF_CAP, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    BYTE* pcm    = (BYTE*)VirtualAlloc(NULL, PCM_PUSH_CAP, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!decBuf || !pcm) {
        if (decBuf) VirtualFree(decBuf, 0, MEM_RELEASE);
        if (pcm)    VirtualFree(pcm, 0, MEM_RELEASE);
        setStatusW(L"内存不足"); setState(RS_ERROR); return;
    }
    int decLen = 0;
    int codec = CODEC_NONE, off = 0;

    /* --- 阶段 1：攒数据探测编码 --- */
    while (!quitNow() && codec == CODEC_NONE) {
        if (decLen < DEC_BUF_CAP)
            decLen += ringRead(decBuf + decLen, DEC_BUF_CAP - decLen);
        if (decLen >= 8 * 1024 || g_netDone || g_netErr) {
            int idsz = id3v2Size(decBuf, decLen);        /* 跳 ID3v2 */
            if (idsz > 0 && decLen > idsz) {
                memmove(decBuf, decBuf + idsz, decLen - idsz);
                decLen -= idsz;
            }
            codec = probeCodec(decBuf, decLen, &off);
            if (codec == CODEC_NONE) {
                if (g_netErr || (g_netDone && ringFill() == 0)) break;
                if (decLen >= 64 * 1024) {   /* 探测窗口上限：丢一半继续 */
                    memmove(decBuf, decBuf + decLen / 2, decLen - decLen / 2);
                    decLen -= decLen / 2;
                }
            }
        }
        if (codec == CODEC_NONE) WaitForSingleObject(g_rbEvt, 100);
    }
    if (quitNow()) goto done;
    if (codec == CODEC_NONE) {
        setStatusW(g_netErr ? L"网络错误" : L"暂不支持该格式（仅支持 MP3 / AAC）");
        setState(RS_ERROR);
        goto done;
    }
    if (off > 0) { memmove(decBuf, decBuf + off, decLen - off); decLen -= off; }

    /* --- 阶段 2：预缓冲攒够再开播 --- */
    setStatusW(L"正在缓冲…");
    setState(RS_BUFFERING);
    ringWaitBytes(thrBytes(PLAY_MS));
    if (quitNow()) goto done;
    if (g_netErr && decLen + ringFill() == 0) {
        setStatusW(L"网络错误"); setState(RS_ERROR); goto done;
    }

    /* --- 阶段 3：解码播放 --- */
    int noProgress = 0, fmtShown = 0;
    while (!quitNow()) {
        if (loopHousekeeping()) break;

        /* 直播缓冲过深：丢弃旧数据贴近直播边缘（解码器按同步字自动重定位） */
        if (ringFill() > NET_BUF_CAP * 9 / 10) {
            int drop = ringFill() - NET_BUF_CAP * 2 / 5;
            ringSkip(drop);
        }

        if (decLen < DEC_BUF_CAP)
            decLen += ringRead(decBuf + decLen, DEC_BUF_CAP - decLen);

        int consumed = 0, pcmBytes = 0;
        const BYTE* pcmSrc = pcm;   /* MP3 解入 pcm；AAC 解入解码器内部缓冲 */

        if (codec == CODEC_MP3) {
            mp3dec_frame_info_t fi;
            memset(&fi, 0, sizeof(fi));
            int samples = mp3dec_decode_frame(&g_mp3, decBuf, decLen,
                                              (mp3d_sample_t*)pcm, &fi);
            consumed = fi.frame_bytes;
            if (samples > 0) {
                if (!fmtShown) {
                    if (fi.bitrate_kbps > 0) g_bps = fi.bitrate_kbps * 125;
                    setFmtInfo(L"MP3", fi.bitrate_kbps, fi.hz, fi.channels);
                    if (ensureAudio(fi.hz, fi.channels) != 0) {
                        setStatusW(L"打开音频设备失败"); setState(RS_ERROR); break;
                    }
                    fmtShown = 1;
                    setState(RS_PLAYING);
                    setStatusW(L"播放中");
                }
                pcmBytes = samples * fi.channels * 2;
                noProgress = 0;
            } else if (consumed > 0) {
                /* 垃圾数据被跳过或帧不完整：丢弃已定位部分 */
                if (consumed > decLen) consumed = decLen;
                noProgress++;
            } else {
                noProgress++;
                if (decLen > DEC_BUF_CAP - 8192) {
                    /* 工作缓冲全满仍无帧：丢弃前半段重新同步 */
                    memmove(decBuf, decBuf + decLen / 2, decLen - decLen / 2);
                    decLen -= decLen / 2;
                    mp3dec_init(&g_mp3);
                }
            }
            if (noProgress > 500) { setStatusW(L"解码无响应，已停止"); setState(RS_ERROR); break; }
            if (consumed <= 0 && pcmBytes == 0) {
                if (starveHandle()) break;
                continue;
            }
        } else {
            /* AAC */
            if (!g_faadInited) {
                unsigned long sr = 0; unsigned char ch = 0;
                long used = NeAACDecInit(g_faad, decBuf, (unsigned long)decLen, &sr, &ch);
                if (used < 0) { setStatusW(L"AAC 初始化失败"); setState(RS_ERROR); break; }
                if (used > 0) {
                    if (used > decLen) used = decLen;
                    memmove(decBuf, decBuf + used, decLen - (int)used);
                    decLen -= (int)used;
                }
                g_faadInited = TRUE;
                setFmtInfo(L"AAC", 0, (int)sr, (int)ch);
                if (ensureAudio((int)sr, (int)ch) != 0) {
                    setStatusW(L"打开音频设备失败"); setState(RS_ERROR); break;
                }
            }
            NeAACDecFrameInfo fi;
            memset(&fi, 0, sizeof(fi));
            void* out = NeAACDecDecode(g_faad, &fi, decBuf, (unsigned long)decLen);
            consumed = (int)fi.bytesconsumed;
            if (fi.error == 0 && fi.samples > 0 && out) {
                if (!fmtShown) {
                    fmtShown = 1;
                    setState(RS_PLAYING);
                    setStatusW(L"播放中");
                }
                pcmBytes = (int)fi.samples * 2;   /* 16bit，samples 含所有声道 */
                pcmSrc = (const BYTE*)out;        /* 解码结果在解码器内部缓冲，
                                                   * 必须推它而不是 pcm（否则全零无声） */
                if (fi.samplerate && fi.channels) {
                    int per = (int)(fi.samples / fi.channels);
                    if (per <= 0) per = 1024;
                    updateBps((long)((LONGLONG)consumed * fi.samplerate / per));
                    /* 用实时估算码率刷新显示（ADTS 头不含码率，初显无 kbps） */
                    int kbps = (int)(((LONGLONG)g_bps * 8 + 500) / 1000);
                    if (kbps != g_fmtKbps) {
                        g_fmtKbps = kbps;
                        setFmtInfo(L"AAC", kbps, (int)fi.samplerate, (int)fi.channels);
                    }
                    if (g_pcmFmt.nSamplesPerSec != fi.samplerate ||
                        g_pcmFmt.nChannels != fi.channels) {
                        g_fmtKbps = 0;   /* 格式变化：清缓存，下帧重新显示估算码率 */
                        setFmtInfo(L"AAC", 0, (int)fi.samplerate, (int)fi.channels);
                    }
                }
                noProgress = 0;
            } else {
                if (consumed <= 0) {
                    /* 失步：跳到下一个 ADTS 同步字 */
                    int i;
                    for (i = 1; i + 1 < decLen; i++)
                        if (decBuf[i] == 0xFF && (decBuf[i + 1] & 0xF0) == 0xF0) break;
                    consumed = (i + 1 < decLen) ? i : decLen;
                }
                noProgress++;
            }
            if (noProgress > 500) { setStatusW(L"解码无响应，已停止"); setState(RS_ERROR); break; }
            if (consumed <= 0 && pcmBytes == 0) {
                if (starveHandle()) break;
                continue;
            }
        }

        /* 消耗输入 */
        if (consumed > 0) {
            if (consumed >= decLen) decLen = 0;
            else { memmove(decBuf, decBuf + consumed, decLen - consumed); decLen -= consumed; }
        }

        /* 提交 PCM */
        if (pcmBytes > 0) {
            if (pcmBytes > PCM_PUSH_CAP) pcmBytes = PCM_PUSH_CAP;
            if (pushPcm(pcmSrc, pcmBytes) != 0) {
                if (!quitNow()) { setStatusW(L"waveOutWrite 失败"); setState(RS_ERROR); }
                break;
            }
        }
    }

done:
    VirtualFree(decBuf, 0, MEM_RELEASE);
    VirtualFree(pcm, 0, MEM_RELEASE);
}

/* ---------------- HLS 线程：分片下载 → TS 解封装 → 灌环形缓冲 ----------------
 * 持续领先播放端下载，天然实现分片预取，片间无缝；
 * 解码侧与流式共用 runDecode 的缓冲状态机。 */
static unsigned __stdcall hlsThread(void* arg) {
    /* 参数为 NetSess*（worker 创建）；take 预缓存通过全局 g_pwTake 传递不再适用，
     * 这里统一由 worker 在启动前把预缓存数据灌入环形缓冲，线程只负责下载。 */
    NetSess* s = (NetSess*)arg;
    Prewarm* take = NULL;   /* HLS 预缓存接管由 worker 完成后传 NULL */
    char plUrl[1024];
    BYTE* plBuf = NULL; int plLen = 0;

    _snprintf(plUrl, sizeof(plUrl) - 1, "%s", g_urlA);
    plUrl[sizeof(plUrl) - 1] = 0;

    /* 跟进主列表变体（最多 4 轮，防自嵌套死循环） */
    HlsPlaylist pl;
    memset(&pl, 0, sizeof(pl));
    int ok = 0;
    for (int round = 0; round < 4 && !quitNow() && s->gen == g_netGen; round++) {
        if (!plBuf && !hlsHttpGet(plUrl, &plBuf, &plLen, 64 * 1024, &g_quit, &s->killReq)) break;
        if (!hlsParsePlaylist((const char*)plBuf, plLen, plUrl, &pl)) break;
        LocalFree(plBuf); plBuf = NULL;
        if (!pl.isMaster) { ok = 1; break; }
        _snprintf(plUrl, sizeof(plUrl) - 1, "%s", pl.variantUrl);
        plUrl[sizeof(plUrl) - 1] = 0;
    }
    if (!ok) {
        if (!quitNow() && s->gen == g_netGen) {
            setStatusW(L"播放列表获取失败");
            InterlockedExchange(&g_netErr, 1);
        }
        if (plBuf) LocalFree(plBuf);
        if (take) prewarmFreeAll(take);
        /* sess 由 worker 统一释放（见 netStreamThread 说明） */
        return 0;
    }

    int nextSeq = pl.mediaSeq + pl.nSegs;
    if (pl.endList && pl.nSegs > 0) nextSeq = pl.mediaSeq;        /* 点播从头 */
    else if (pl.nSegs >= 2) nextSeq = pl.mediaSeq + pl.nSegs - 2; /* 直播留一片预取余量 */

    static BYTE esBuf[HLS_ES_CAP];
    for (;;) {
        if (quitNow() || s->gen != g_netGen) break;
        BYTE* pb = NULL; int pn = 0;
        if (!hlsHttpGet(plUrl, &pb, &pn, 64 * 1024, &g_quit, &s->killReq)) {
            if (quitNow() || s->gen != g_netGen) break;
            quitSleep(1000); continue;   /* 列表刷新失败稍后重试 */
        }
        HlsPlaylist pl2;
        if (!hlsParsePlaylist((const char*)pb, pn, plUrl, &pl2)) {
            LocalFree(pb);
            if (!quitNow()) quitSleep(1000);
            continue;
        }
        LocalFree(pb);

        for (int i = 0; i < pl2.nSegs && !quitNow() && s->gen == g_netGen; i++) {
            int seq = pl2.mediaSeq + i;
            if (seq < nextSeq) continue;
            nextSeq = seq + 1;

            BYTE* seg = NULL; int segLen = 0;
            if (!hlsHttpGet(pl2.segUrl[i], &seg, &segLen, HLS_SEG_MAX, &g_quit, &s->killReq)) {
                if (quitNow() || s->gen != g_netGen) break;
                nextSeq = seq;      /* 下一轮重试此分片 */
                break;
            }

            TsCtx ts; tsInit(&ts);
            int esLen = tsFeed(&ts, seg, segLen, esBuf, HLS_ES_CAP, 0);
            LocalFree(seg);
            if (ts.esPid < 0 || esLen <= 0) continue;
            if (ts.esType == 0x11) {
                setStatusW(L"暂不支持 LATM AAC");
                InterlockedExchange(&g_netErr, 1);
                goto out;
            }
            if (ts.esType != 0x03 && ts.esType != 0x04 && ts.esType != 0x0F)
                continue;
            if (sessRingWrite(s, esBuf, esLen) < 0) goto out;
        }

        if (quitNow() || s->gen != g_netGen) break;
        if (pl2.endList) { InterlockedExchange(&g_netDone, 1); break; }
        int waitMs = pl2.targetDur * 500; if (waitMs < 500) waitMs = 500;
        quitSleep(waitMs);
    }
out:
    if (take) prewarmFreeAll(take);
    /* sess 由 worker 统一释放（见 netStreamThread 说明） */
    return 0;
}

/* ---------------- 工作线程 ---------------- */
static BOOL isPlaylistExt(const char* url, int* isPls) {
    size_t n = strlen(url);
    *isPls = 0;
    if (n > 4 && _stricmp(url + n - 4, ".pls") == 0) { *isPls = 1; return TRUE; }
    if (n > 5 && _stricmp(url + n - 5, ".m3u8") == 0) return TRUE;
    if (n > 4 && _stricmp(url + n - 4, ".m3u") == 0)  return TRUE;
    return FALSE;
}
static BOOL isPlaylistCt(const char* ct) {
    return (_strnicmp(ct, "application/vnd.apple.mpegurl", 27) == 0 ||
            _strnicmp(ct, "application/x-mpegURL", 19) == 0 ||
            _strnicmp(ct, "audio/x-mpegurl", 15) == 0 ||
            _strnicmp(ct, "audio/mpegurl", 13) == 0);
}

/* ---------------- 预缓存（切台即时开播） ----------------
 * 播放当前台的同时，后台线程提前为目标台建连并缓冲数据；
 * 用户切到该台时由 worker 直接接管连接与已缓冲数据。
 * 同时只有一个预缓存目标，只由 UI 线程操作 g_pw。 */
static Prewarm* g_pw = NULL;

static void prewarmFreeAll(Prewarm* pw) {
    if (!pw) return;
    if (pw->hReq)  InternetCloseHandle(pw->hReq);
    if (pw->hConn) InternetCloseHandle(pw->hConn);
    if (pw->hInet) InternetCloseHandle(pw->hInet);
    if (pw->buf)   free(pw->buf);
    if (pw->plBuf) LocalFree(pw->plBuf);
    if (pw->segBuf) LocalFree(pw->segBuf);
    if (pw->thread) CloseHandle(pw->thread);
    free(pw);
}

/* HLS 预缓存：跟进嵌套主列表，缓存媒体列表与最后一片 */
static void prewarmHls(Prewarm* pw) {
    char plUrl[1024];
    _snprintf(plUrl, sizeof(plUrl) - 1, "%s", pw->urlA);
    plUrl[sizeof(plUrl) - 1] = 0;
    pw->isHls = 1;
    for (int round = 0; round < 4 && !pw->quit; round++) {
        BYTE* buf = NULL; int len = 0;
        if (!hlsHttpGet(plUrl, &buf, &len, 64 * 1024, &pw->quit, NULL)) return;
        HlsPlaylist pl;
        if (!hlsParsePlaylist((const char*)buf, len, plUrl, &pl)) {
            LocalFree(buf); return;
        }
        if (pl.isMaster) {
            _snprintf(plUrl, sizeof(plUrl) - 1, "%s", pl.variantUrl);
            plUrl[sizeof(plUrl) - 1] = 0;
            LocalFree(buf);
            continue;
        }
        _snprintf(pw->plUrl, sizeof(pw->plUrl) - 1, "%s", plUrl);
        pw->plUrl[sizeof(pw->plUrl) - 1] = 0;
        pw->plBuf = buf; pw->plLen = len;
        InterlockedExchange(&pw->ready, 1);
        if (pl.nSegs > 0 && !pw->quit) {
            const char* su = pl.segUrl[pl.nSegs - 1];
            BYTE* sb = NULL; int sl = 0;
            if (hlsHttpGet(su, &sb, &sl, HLS_SEG_MAX, &pw->quit, NULL) && sl > 0) {
                _snprintf(pw->segUrl, sizeof(pw->segUrl) - 1, "%s", su);
                pw->segUrl[sizeof(pw->segUrl) - 1] = 0;
                pw->segBuf = sb; pw->segLen = sl;
            }
        }
        return;
    }
}

static unsigned __stdcall prewarmThread(void* arg) {
    Prewarm* pw = (Prewarm*)arg;
    int isPls = 0;
    if (isPlaylistExt(pw->urlA, &isPls)) {
        if (!isPls) prewarmHls(pw);   /* .pls 不支持，直接放弃 */
    } else {
        char ct[128] = {0};
        if (httpOpenEx(&pw->hInet, &pw->hConn, &pw->hReq, pw->urlA,
                       &pw->icy, ct, sizeof(ct), 8000, TRUE)) {
            _snprintf(pw->ct, sizeof(pw->ct) - 1, "%s", ct);
            if (isPlaylistCt(ct)) {
                /* 伪装成流的播放列表：转 HLS 预缓存 */
                InternetCloseHandle(pw->hReq);  pw->hReq  = NULL;
                InternetCloseHandle(pw->hConn); pw->hConn = NULL;
                InternetCloseHandle(pw->hInet); pw->hInet = NULL;
                prewarmHls(pw);
            } else {
                /* 流式：持续填充缓冲，达到上限后退出（连接保持） */
                while (!pw->quit && pw->bufLen < PW_BUF_CAP) {
                    int space = PW_BUF_CAP - pw->bufLen;
                    if (space > 16 * 1024) space = 16 * 1024;
                    int got = icyReadEx(pw->hReq, &pw->icy, &pw->quit,
                                        pw->buf + pw->bufLen, space);
                    if (got <= 0) break;
                    pw->bufLen += got;
                    if (!pw->ready && pw->bufLen >= PW_READY_MIN)
                        InterlockedExchange(&pw->ready, 1);
                }
                if (pw->bufLen > 0) InterlockedExchange(&pw->ready, 1);
            }
        }
    }
    if (!pw->claim) prewarmFreeAll(pw);   /* 无人认领：自行清理 */
    return 0;
}

/* 放弃当前预缓存：置退出标志后不等待，线程自行清理（不阻塞切台） */
static void prewarmAbandon(void) {
    Prewarm* pw = g_pw;
    g_pw = NULL;
    if (pw) InterlockedExchange(&pw->quit, 1);
}

void radio_prewarm(const char* utf8Url) {
    if (!utf8Url || !*utf8Url) return;
    /* 不预缓存正在播放的台 */
    char curA[1024] = {0};
    lock();
    WideCharToMultiByte(CP_UTF8, 0, g_info.url, -1, curA, sizeof(curA) - 1, NULL, NULL);
    unlock();
    if (curA[0] && strcmp(curA, utf8Url) == 0) return;
    if (g_pw && strcmp(g_pw->urlA, utf8Url) == 0) return;   /* 已在预热同一目标 */
    prewarmAbandon();
    Prewarm* pw = (Prewarm*)calloc(1, sizeof(Prewarm));
    if (!pw) return;
    _snprintf(pw->urlA, sizeof(pw->urlA) - 1, "%s", utf8Url);
    pw->icy.silent = 1;
    pw->buf = (BYTE*)malloc(PW_BUF_CAP);
    if (!pw->buf) { free(pw); return; }
    unsigned tid = 0;
    /* 先挂起创建，存好句柄再恢复，避免线程抢在句柄保存前退出 */
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, prewarmThread, pw,
                                      CREATE_SUSPENDED, &tid);
    if (!h) { free(pw->buf); free(pw); return; }
    pw->thread = h;
    g_pw = pw;
    ResumeThread(h);
}

/* 新 worker 启动参数：携带预缓存接管对象与上一代 worker 线程句柄。
 * 切台时由 UI 线程把旧 worker 句柄打包交给新 worker，新 worker 在自己的
 * 线程里 join 旧 worker（不阻塞 UI），确认旧会话完全清理后再分配资源。 */
typedef struct {
    Prewarm* take;
    HANDLE   oldThread;
} StartArg;

static unsigned __stdcall worker(void* arg) {
    StartArg* sa = (StartArg*)arg;
    Prewarm* take = sa ? sa->take : NULL;
    HANDLE   oldThread = sa ? sa->oldThread : NULL;
    if (sa) free(sa);

    /* 先等待上一代 worker 完全退出（在本工作线程内等待，UI 不卡顿）。
     * 旧 worker 收到 quit 后会强制中断网络并释放其全部全局资源；
     * 必须等它退出，两代 worker 才能安全交接 g_rb/g_hWo/g_faad 等全局状态。 */
    if (oldThread) {
        InterlockedExchange(&g_quit, 1);
        if (g_hWake)  SetEvent(g_hWake);
        if (g_rbEvt)  SetEvent(g_rbEvt);
        if (g_hWo)    waveOutReset(g_hWo);
        /* 旧 worker 通常在 2~5 秒内自行退出（含强制中断网络） */
        if (WaitForSingleObject(oldThread, 6000) == WAIT_TIMEOUT) {
            /* 极少数情况下旧线程卡死：再给它时间，但不无限等。
             * 无论如何继续（旧线程即便残留也因代际失效而不再触碰新资源）。 */
            WaitForSingleObject(oldThread, 4000);
        }
        CloseHandle(oldThread);
        /* 旧 worker 退出时会清 g_active/g_threadRun，这里重新确立新会话状态 */
        InterlockedExchange(&g_active, 1);
    }

    /* 交接完成：清除上一代的退出标志，新会话开始正常运行 */
    InterlockedExchange(&g_quit, 0);
    InterlockedExchange(&g_threadRun, 1);

    char urlA[1024] = {0};
    {
        lock();
        WideCharToMultiByte(CP_UTF8, 0, g_info.url, -1, urlA, sizeof(urlA) - 1, NULL, NULL);
        unlock();
    }
    if (!urlA[0]) {
        setStatusW(L"URL 无效"); setState(RS_ERROR);
        prewarmFreeAll(take);
        goto done;
    }
    _snprintf(g_urlA, sizeof(g_urlA) - 1, "%s", urlA);
    g_urlA[sizeof(g_urlA) - 1] = 0;

    decodersReset();
    g_hWoEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!faadPrepare()) {   /* HLS 与流式路径都需要，必须提前初始化 */
        setStatusW(L"初始化解码器失败");
        setState(RS_ERROR);
        prewarmFreeAll(take); take = NULL;
        goto done;
    }

    /* 压缩数据环形缓冲（网络线程与解码线程之间的解耦层） */
    g_rb = (BYTE*)VirtualAlloc(NULL, NET_BUF_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_rbEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
    ringReset();
    InterlockedExchange(&g_netErr, 0);
    InterlockedExchange(&g_netDone, 0);
    g_bps = 12000;
    if (!g_rb || !g_rbEvt) {
        setStatusW(L"内存不足"); setState(RS_ERROR);
        prewarmFreeAll(take); take = NULL;
        goto done;
    }

    setState(RS_CONNECTING);
    setStatusW(L"正在连接…");

    /* 本会话代际：递增后，任何上一代残留网络线程的写环操作立即失效，
     * 它们绝不会碰到本会话新分配的 g_rb / g_rbEvt。 */
    LONG myGen = InterlockedIncrement(&g_gen);
    InterlockedExchange(&g_netGen, myGen);

    /* 网络线程会话上下文（堆分配；网络线程退出后由 worker 在 done 段统一释放，
     * 以避免 worker 强制中断时访问悬垂指针） */
    NetSess* sess = (NetSess*)calloc(1, sizeof(NetSess));
    if (!sess) { setStatusW(L"内存不足"); setState(RS_ERROR); prewarmFreeAll(take); take = NULL; goto done; }
    sess->rb = g_rb; sess->rbEvt = g_rbEvt; sess->gen = myGen;
    sess->hReq = NULL; sess->killReq = NULL;
    g_killSess = sess;   /* 供 done 段强制中断 HLS/流式句柄 */

    /* ---- 分发：确定 HLS 还是流式，启动网络线程 ---- */
    int hls = 0;
    {
        int isPls = 0;
        if (isPlaylistExt(urlA, &isPls)) {
            if (isPls) {
                setStatusW(L"暂不支持 PLS 格式"); setState(RS_ERROR);
                prewarmFreeAll(take); take = NULL; free(sess); g_killSess = NULL;
                goto done;
            }
            hls = 1;
        } else if (take && take->isHls) {
            hls = 1;   /* 预缓存按 Content-Type 探测出是 HLS */
        }
    }

    /* HLS 预缓存数据由 HLS 线程自行重新拉取（媒体列表短，代价小），释放 take */
    if (hls && take) { prewarmFreeAll(take); take = NULL; }

    if (hls) {
        unsigned tid = 0;
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, hlsThread, sess, 0, &tid);
        if (!h) { setStatusW(L"线程创建失败"); setState(RS_ERROR); free(sess); g_killSess = NULL; goto done; }
        g_netThread = h; sess = NULL;   /* 所有权移交网络线程 */
    } else {
        /* ---- 流式：接管预缓存或新建连接，同步探测类型（在工作线程，不卡 UI）---- */
        IcyCtx icy; memset(&icy, 0, sizeof(icy));
        char ct[128] = {0};
        HINTERNET hInet = NULL, hConn = NULL, hReq = NULL;
        if (take) {
            hInet = take->hInet; hConn = take->hConn; hReq = take->hReq;
            take->hInet = take->hConn = take->hReq = NULL;
            icy = take->icy; icy.silent = 0;
            _snprintf(ct, sizeof(ct) - 1, "%s", take->ct);
            if (take->bufLen > 0) ringWrite(take->buf, take->bufLen);
            prewarmFreeAll(take); take = NULL;
        } else {
            if (!httpOpenEx(&hInet, &hConn, &hReq, urlA, &icy, ct, sizeof(ct), 8000, FALSE)) {
                if (g_info.status[0] == 0) setStatusW(L"连接失败");
                setState(RS_ERROR);
                if (hReq) InternetCloseHandle(hReq);
                if (hConn) InternetCloseHandle(hConn);
                if (hInet) InternetCloseHandle(hInet);
                free(sess); g_killSess = NULL;
                goto done;
            }
            /* 预读首块，识别伪装成流的播放列表 */
            BYTE head[1024];
            int got = icyReadEx(hReq, &icy, &g_quit, head, sizeof(head));
            if (got <= 0) {
                setStatusW(got == 0 ? L"流已关闭" : L"网络读取失败");
                setState(RS_ERROR);
                InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hInet);
                free(sess); g_killSess = NULL;
                goto done;
            }
            if (got >= 7 && memcmp(head, "#EXTM3U", 7) == 0)
                _snprintf(ct, sizeof(ct) - 1, "%s", "application/vnd.apple.mpegurl");
            ringWrite(head, got);
        }

        unsigned tid = 0;
        if (isPlaylistCt(ct)) {
            /* 伪装成流的 HLS：关掉流式连接，交给 HLS 线程重新拉取 */
            if (hReq) InternetCloseHandle(hReq);
            if (hConn) InternetCloseHandle(hConn);
            if (hInet) InternetCloseHandle(hInet);
            HANDLE h = (HANDLE)_beginthreadex(NULL, 0, hlsThread, sess, 0, &tid);
            if (!h) { setStatusW(L"线程创建失败"); setState(RS_ERROR); free(sess); g_killSess = NULL; goto done; }
            g_netThread = h; sess = NULL;
        } else {
            /* 把已建连的整套句柄移交给网络线程（它负责关闭与重连）。
             * 注意 WinINet 中关闭父句柄 hInet/hConn 会连带关闭 hReq，
             * 故三者必须一起交给网络线程，不能在这里提前关闭。 */
            sess->hInet = hInet; sess->hConn = hConn; sess->hReq = hReq;
            HANDLE h = (HANDLE)_beginthreadex(NULL, 0, netStreamThread, sess, 0, &tid);
            if (!h) {
                setStatusW(L"线程创建失败"); setState(RS_ERROR);
                if (hReq) InternetCloseHandle(hReq);
                if (hConn) InternetCloseHandle(hConn);
                if (hInet) InternetCloseHandle(hInet);
                free(sess); g_killSess = NULL;
                goto done;
            }
            g_netThread = h; sess = NULL;
        }
    }

    runDecode();

done:
    {
        int wasQuit = quitNow();
        InterlockedExchange(&g_quit, 1);   /* 通知网络线程退出 */
        /* 递增代际：让仍在运行的旧网络线程立即停止写环并自行退出，
         * 这样它绝不会再触碰下面即将释放的 g_rb / g_rbEvt。 */
        InterlockedIncrement(&g_gen);
        InterlockedIncrement(&g_netGen);
        if (g_rbEvt) SetEvent(g_rbEvt);
        if (g_hWake) SetEvent(g_hWake);
        if (g_netThread) {
            /* 先等优雅退出；超时则强制关闭正在阻塞的 WinINet 句柄解除阻塞。
             * 网络线程通过 NetSess 持有句柄，关闭后其 InternetReadFile/
             * HttpSendRequest 会立刻失败返回，线程随之退出。 */
            if (WaitForSingleObject(g_netThread, 2000) == WAIT_TIMEOUT) {
                NetSess* ks = g_killSess;
                if (ks) {
                    if (ks->killReq) { InternetCloseHandle((HINTERNET)ks->killReq); ks->killReq = NULL; }
                    if (ks->hReq)    { InternetCloseHandle(ks->hReq); ks->hReq = NULL; }
                    if (ks->hConn)   { InternetCloseHandle(ks->hConn); ks->hConn = NULL; }
                    if (ks->hInet)   { InternetCloseHandle(ks->hInet); ks->hInet = NULL; }
                }
                /* 兜底：清理任何残留在全局变量里的句柄 */
                if (g_hReq)  { InternetCloseHandle(g_hReq);  g_hReq  = NULL; }
                if (g_hConn) { InternetCloseHandle(g_hConn); g_hConn = NULL; }
                if (g_hInet) { InternetCloseHandle(g_hInet); g_hInet = NULL; }
                WaitForSingleObject(g_netThread, 3000);
            }
            CloseHandle(g_netThread); g_netThread = NULL;
        }
        /* 网络线程已 join：sess 此刻无人使用，安全释放（句柄已被网络线程
         * 自行关闭或在上面的强制中断中关闭）。 */
        if (g_killSess) { free(g_killSess); g_killSess = NULL; }
        /* 全局句柄兜底清理（正常路径网络线程已自行关闭） */
        if (g_hReq)  { InternetCloseHandle(g_hReq);  g_hReq  = NULL; }
        if (g_hConn) { InternetCloseHandle(g_hConn); g_hConn = NULL; }
        if (g_hInet) { InternetCloseHandle(g_hInet); g_hInet = NULL; }
        woClose();
        if (take) prewarmFreeAll(take);
        if (g_faad) { NeAACDecClose(g_faad); g_faad = NULL; g_faadInited = FALSE; }
        if (g_hWoEvt) { CloseHandle(g_hWoEvt); g_hWoEvt = NULL; }
        /* 网络线程已确认退出（上面 join 过），此时释放环形缓冲安全，无 use-after-free */
        if (g_rbEvt) { CloseHandle(g_rbEvt); g_rbEvt = NULL; }
        if (g_rb) { VirtualFree(g_rb, 0, MEM_RELEASE); g_rb = NULL; }
        if (wasQuit) {
            setState(RS_STOPPED);
            setStatusW(L"已停止");
        }
    }
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_threadRun, 0);
    return 0;
}

/* ---------------- 对外 API ---------------- */
void radio_startup(void) {
    if (!g_lockInited) { InitializeCriticalSection(&g_lock); g_lockInited = TRUE; }
    resetInfo(NULL);
    if (!g_hWake) g_hWake = CreateEvent(NULL, FALSE, FALSE, NULL);
}
void radio_cleanup(void) {
    radio_stop();
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_hWake)  { CloseHandle(g_hWake); g_hWake = NULL; }
}

BOOL radio_play(const char* utf8Url, const WCHAR* display) {
    (void)display;
    if (!utf8Url || !*utf8Url) return FALSE;

    /* 预缓存命中：停掉预热线程并接管；否则放弃预热 */
    Prewarm* take = NULL;
    if (g_pw) {
        if (g_pw->ready && strcmp(g_pw->urlA, utf8Url) == 0) {
            Prewarm* pw = g_pw;
            InterlockedExchange(&pw->claim, 1);
            InterlockedExchange(&pw->quit, 1);
            DWORD waited = 0;
            while (WaitForSingleObject(pw->thread, 100) == WAIT_TIMEOUT) {
                waited += 100;
                if (waited >= 4000) break;
            }
            if (waited < 4000) {
                take = pw;
            } else {
                /* 线程未及时退出：放弃接管，归还所有权由其自清理 */
                InterlockedExchange(&pw->claim, 0);
            }
        } else {
            prewarmAbandon();
        }
        g_pw = NULL;
    }

    /* 切台：不在 UI 线程等待旧 worker（连接不畅时旧 worker 可能要几秒才能
     * 强制中断网络并退出，长等会让界面卡死）。改为把旧 worker 句柄打包交给
     * 新 worker，由新 worker 在自己的后台线程里 join 旧 worker 后再开播。
     * UI 线程立即返回，界面不再卡顿。 */
    HANDLE oldThread = NULL;
    if (g_thread) {
        InterlockedExchange(&g_quit, 1);
        if (g_hWake)  SetEvent(g_hWake);
        if (g_rbEvt)  SetEvent(g_rbEvt);
        if (g_hWo)    waveOutReset(g_hWo);
        oldThread = g_thread;   /* 所有权移交新 worker 去 CloseHandle */
        g_thread = NULL;
    }

    WCHAR urlW[512] = {0};
    if (MultiByteToWideChar(CP_UTF8, 0, utf8Url, -1, urlW, 511) <= 0) {
        if (MultiByteToWideChar(CP_ACP, 0, utf8Url, -1, urlW, 511) <= 0) {
            if (oldThread) CloseHandle(oldThread);
            return FALSE;
        }
    }

    resetInfo(urlW);
    InterlockedExchange(&g_quit, 0);
    InterlockedExchange(&g_pause, 0);
    InterlockedExchange(&g_active, 1);

    StartArg* sa = (StartArg*)calloc(1, sizeof(StartArg));
    if (!sa) {
        if (oldThread) CloseHandle(oldThread);
        prewarmFreeAll(take);
        return FALSE;
    }
    sa->take = take;
    sa->oldThread = oldThread;

    unsigned tid = 0;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, worker, sa, 0, &tid);
    if (!h) {
        if (take && take->thread) { CloseHandle(take->thread); take->thread = NULL; }
        prewarmFreeAll(take);
        if (oldThread) CloseHandle(oldThread);
        free(sa);
        return FALSE;
    }
    g_thread = h;
    return TRUE;
}

void radio_stop(void) {
    prewarmAbandon();
    InterlockedExchange(&g_quit, 1);
    InterlockedExchange(&g_pause, 0);
    if (g_hWake) SetEvent(g_hWake);
    if (g_rbEvt) SetEvent(g_rbEvt);
    if (g_hWo)   waveOutReset(g_hWo);
}

void radio_set_paused(BOOL paused) {
    InterlockedExchange(&g_pause, paused ? 1 : 0);
    if (g_hWo) {
        if (paused) waveOutPause(g_hWo);
        else        waveOutRestart(g_hWo);
    }
    lock();
    if (g_info.state == RS_PLAYING || g_info.state == RS_PAUSED) {
        g_info.state = paused ? RS_PAUSED : RS_PLAYING;
        copyW(g_info.status, 160, paused ? L"已暂停" : L"播放中");
    }
    unlock();
    if (g_hWake) SetEvent(g_hWake);
}
BOOL radio_is_paused(void) { return InterlockedCompareExchange(&g_pause, 0, 0) ? TRUE : FALSE; }
BOOL radio_is_active(void) { return InterlockedCompareExchange(&g_active, 0, 0) ? TRUE : FALSE; }

void radio_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volPct = (DWORD)pct;
    if (!g_hWo) return;
    DWORD v = (DWORD)(pct * 655.35);
    if (v > 0xFFFF) v = 0xFFFF;
    waveOutSetVolume(g_hWo, MAKELONG(v, v));
}
int radio_get_volume(void) { return (int)g_volPct; }

void radio_get_info(RADIO_INFO* out) {
    lock(); *out = g_info; unlock();
}
