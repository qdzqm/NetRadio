/* hls.c - m3u8 播放列表解析 + MPEG-TS 解封装（提取音频基本流）
 *
 * 参考公开格式规范：RFC 8216 (HLS)、ISO/IEC 13818-1 (MPEG-TS)。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hls.h"

/* 给请求句柄设置连接/收发超时，避免无限阻塞 */
static void hlsSetTimeouts(HINTERNET hR) {
    DWORD ms = 8000;
    InternetSetOptionA(hR, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
    InternetSetOptionA(hR, INTERNET_OPTION_SEND_TIMEOUT,    &ms, sizeof(ms));
    InternetSetOptionA(hR, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
}

/* ---------------- 一次性 HTTP GET ---------------- */
int hlsHttpGet(const char* url, BYTE** outBuf, int* outLen, int maxLen,
               volatile LONG* quit, volatile HINTERNET* outReq) {
#define hlsQuit() (quit && *(quit) != 0)
    int ok = 0;
    HINTERNET hI = NULL, hC = NULL, hR = NULL;
    BYTE* buf = NULL;
    int len = 0;

    *outBuf = NULL; *outLen = 0;

    URL_COMPONENTSA uc;
    char hostA[256] = {0}, pathA[768] = {0}, extraA[256] = {0};
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = hostA;  uc.dwHostNameLength  = sizeof(hostA) - 1;
    uc.lpszUrlPath      = pathA;  uc.dwUrlPathLength   = sizeof(pathA) - 1;
    uc.lpszExtraInfo    = extraA; uc.dwExtraInfoLength = sizeof(extraA) - 1;
    if (!InternetCrackUrlA(url, 0, 0, &uc)) return 0;

    /* DIRECT：不走系统代理自动检测（WPAD），避免每次连接空转数秒 */
    hI = InternetOpenA("NetRadio/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hI) return 0;
    hC = InternetConnectA(hI, hostA, uc.nPort, NULL, NULL,
                          INTERNET_SERVICE_HTTP, 0, 0);
    if (!hC) goto done;

    char full[1024];
    if (extraA[0]) _snprintf(full, sizeof(full) - 1, "%s%s", pathA, extraA);
    else           _snprintf(full, sizeof(full) - 1, "%s", pathA);
    full[sizeof(full) - 1] = 0;

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS)
        flags |= INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID
                             | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;

    hR = HttpOpenRequestA(hC, "GET", full, "HTTP/1.1", NULL, NULL, flags, 0);
    if (!hR) goto done;
    hlsSetTimeouts(hR);
    if (outReq) InterlockedExchangePointer((volatile PVOID*)outReq, (PVOID)hR);  /* 登记，供外部强制中断 */
    if (!HttpSendRequestA(hR, NULL, 0, NULL, 0)) goto done;

    char status[32]; DWORD szl = sizeof(status);
    if (HttpQueryInfoA(hR, HTTP_QUERY_STATUS_CODE, status, &szl, NULL)) {
        int code = atoi(status);
        if (code < 200 || code >= 300) goto done;
    }

    buf = (BYTE*)LocalAlloc(LMEM_FIXED, maxLen + 1);
    if (!buf) goto done;
    for (;;) {
        if (hlsQuit()) break;      /* 切台：中止下载，返回已收数据按失败处理 */
        if (len >= maxLen) break;
        DWORD g = 0;
        if (!InternetReadFile(hR, buf + len, (DWORD)(maxLen - len), &g)) break;
        if (g == 0) break;
        len += (int)g;
    }
    buf[len] = 0;
    *outBuf = buf; *outLen = len; buf = NULL;
    ok = (len > 0) && !hlsQuit();
done:
    if (outReq) InterlockedExchangePointer((volatile PVOID*)outReq, NULL);   /* 注销登记 */
    if (buf) LocalFree(buf);
    if (hR) InternetCloseHandle(hR);
    if (hC) InternetCloseHandle(hC);
    if (hI) InternetCloseHandle(hI);
#undef hlsQuit
    return ok;
}

/* ---------------- URL 解析 ---------------- */
/* 取 base 的目录部分（去掉最后一段文件名） */
static void baseDir(const char* base, char* out, int cap) {
    int i = (int)strlen(base) - 1;
    while (i > 0 && base[i] != '/') i--;
    int n = i + 1;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
}

void hlsResolveUrl(const char* baseUrl, const char* rel, char* out, int cap) {
    if (_strnicmp(rel, "http://", 7) == 0 || _strnicmp(rel, "https://", 8) == 0) {
        _snprintf(out, cap - 1, "%s", rel); out[cap - 1] = 0;
        return;
    }
    if (rel[0] == '/') {
        /* 绝对路径：取 scheme://host[:port] */
        const char* p = strstr(baseUrl, "://");
        p = p ? p + 3 : baseUrl;
        const char* slash = strchr(p, '/');
        int n = slash ? (int)(slash - baseUrl) : (int)strlen(baseUrl);
        if (n > cap - 2) n = cap - 2;
        memcpy(out, baseUrl, n);
        _snprintf(out + n, cap - n - 1, "%s", rel);
        out[cap - 1] = 0;
        return;
    }
    /* 相对路径 */
    char dir[1024];
    baseDir(baseUrl, dir, sizeof(dir));
    char tmp[1024];
    _snprintf(tmp, sizeof(tmp) - 1, "%s%s", dir, rel);
    tmp[sizeof(tmp) - 1] = 0;

    /* 消除 ../ 和 ./ */
    char* seg = tmp;
    for (;;) {
        char* dotdot = strstr(seg, "/../");
        if (!dotdot) break;
        /* 找 ../ 之前的上一级目录起点 */
        char* prev = dotdot;
        if (prev > tmp) prev--;               /* 跳过当前 '/' */
        while (prev > tmp && *prev != '/') prev--;
        if (*prev == '/') prev++;
        memmove(prev, dotdot + 4, strlen(dotdot + 4) + 1);
        seg = prev;
    }
    char* dot1 = strstr(tmp, "/./");
    while (dot1) {
        memmove(dot1 + 1, dot1 + 3, strlen(dot1 + 3) + 1);
        dot1 = strstr(tmp, "/./");
    }
    _snprintf(out, cap - 1, "%s", tmp);
    out[cap - 1] = 0;
}

/* ---------------- m3u8 解析 ---------------- */
int hlsParsePlaylist(const char* text, int textLen, const char* baseUrl,
                     HlsPlaylist* pl) {
    memset(pl, 0, sizeof(*pl));
    pl->targetDur = 6;
    if (textLen <= 0) return 0;

    char line[1024];
    int pos = 0;
    int pendingInf = 0;   /* 上一行是 #EXT-X-STREAM-INF */

    while (pos < textLen) {
        int e = pos;
        while (e < textLen && text[e] != '\n' && text[e] != '\r') e++;
        int ln = e - pos;
        if (ln > (int)sizeof(line) - 1) ln = (int)sizeof(line) - 1;
        memcpy(line, text + pos, ln);
        line[ln] = 0;
        /* 下一行起点 */
        pos = e;
        while (pos < textLen && (text[pos] == '\n' || text[pos] == '\r')) pos++;

        if (ln == 0) continue;
        if (line[0] == '#') {
            if (_strnicmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0)
                pl->mediaSeq = atoi(line + 22);
            else if (_strnicmp(line, "#EXT-X-TARGETDURATION:", 22) == 0)
                pl->targetDur = atoi(line + 22);
            else if (_strnicmp(line, "#EXT-X-ENDLIST", 14) == 0)
                pl->endList = 1;
            else if (_strnicmp(line, "#EXT-X-STREAM-INF", 17) == 0)
                pendingInf = 1;
            continue;
        }
        if (pendingInf) {
            /* 主播放列表：记录第一个变体 */
            pl->isMaster = 1;
            if (!pl->variantUrl[0])
                hlsResolveUrl(baseUrl, line, pl->variantUrl, sizeof(pl->variantUrl));
            pendingInf = 0;
            continue;
        }
        /* 分片行 */
        if (pl->nSegs < HLS_MAX_SEGS) {
            hlsResolveUrl(baseUrl, line, pl->segUrl[pl->nSegs],
                          sizeof(pl->segUrl[0]));
            pl->nSegs++;
        }
    }
    if (pl->targetDur <= 0) pl->targetDur = 6;
    return (pl->isMaster || pl->nSegs > 0);
}

/* ---------------- MPEG-TS 解封装 ---------------- */
void tsInit(TsCtx* c) { memset(c, 0, sizeof(*c)); c->pmtPid = -1; c->esPid = -1; }

/* 解析 PAT → 得到 PMT PID */
static void parsePat(TsCtx* c, const BYTE* s, int len) {
    /* s: section 起始 (table_id) */
    if (len < 12 || s[0] != 0x00) return;
    int secLen = ((s[1] & 0x0F) << 8) | s[2];
    if (secLen > len - 3) secLen = len - 3;
    int end = 3 + secLen - 4;   /* 去尾部 CRC */
    for (int i = 8; i + 3 < end; i += 4) {
        int progNum = (s[i] << 8) | s[i + 1];
        if (progNum != 0) {
            c->pmtPid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
            return;
        }
    }
}

/* 解析 PMT → 得到音频 ES PID/类型 */
static void parsePmt(TsCtx* c, const BYTE* s, int len) {
    if (len < 16 || s[0] != 0x02) return;
    int secLen = ((s[1] & 0x0F) << 8) | s[2];
    if (secLen > len - 3) secLen = len - 3;
    int end = 3 + secLen - 4;
    int piLen = ((s[10] & 0x0F) << 8) | s[11];
    int i = 12 + piLen;
    while (i + 4 < end) {
        int st   = s[i];
        int pid  = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
        int esLen = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
        /* 音频类型：0x03/0x04=MP3, 0x0F=AAC ADTS, 0x11=AAC LATM, 0x81=AC3 */
        if (st == 0x03 || st == 0x04 || st == 0x0F || st == 0x11) {
            c->esPid = pid;
            c->esType = st;
            return;
        }
        i += 5 + esLen;
    }
}

static int esAppend(TsCtx* c, const BYTE* p, int n, BYTE* esOut, int esCap,
                    int filled) {
    (void)c;
    int space = esCap - filled;
    if (n > space) n = space;
    if (n > 0) memcpy(esOut + filled, p, n);
    return n;
}

int tsFeed(TsCtx* c, const BYTE* data, int len, BYTE* esOut, int esCap,
           int esFilled) {
    int written = 0;

    /* 对齐到 0x47 同步字 */
    int off = 0;
    while (off < len && data[off] != 0x47) off++;
    if (off >= len) return 0;

    for (; off + 188 <= len; off += 188) {
        const BYTE* p = data + off;
        if (p[0] != 0x47) break;
        int pusi = (p[1] & 0x40) != 0;
        int pid  = ((p[1] & 0x1F) << 8) | p[2];
        int adapt = (p[3] >> 4) & 3;
        int plOff = 4;
        if (adapt & 2) {
            int alen = p[4];
            plOff = 5 + alen;
        }
        if (!(adapt & 1) || plOff >= 188) continue;   /* 无负载 */
        const BYTE* pl = p + plOff;
        int plLen = 188 - plOff;

        if (pid == 0 && pusi) {
            int ptr = pl[0];
            if (1 + ptr < plLen) parsePat(c, pl + 1 + ptr, plLen - 1 - ptr);
        } else if (c->pmtPid >= 0 && pid == c->pmtPid && pusi) {
            int ptr = pl[0];
            if (1 + ptr < plLen) parsePmt(c, pl + 1 + ptr, plLen - 1 - ptr);
        } else if (c->esPid >= 0 && pid == c->esPid) {
            /* PES 重组：PUSI 包含 PES 头 */
            if (pusi) {
                c->sawPusi = 1;
                c->pesSkip = 0;
                if (plLen > 9 && pl[0] == 0 && pl[1] == 0 && pl[2] == 1) {
                    int hdrDataLen = pl[8];
                    int esStart = 9 + hdrDataLen;
                    if (esStart < plLen) {
                        int n = esAppend(c, pl + esStart, plLen - esStart,
                                         esOut, esCap, esFilled + written);
                        written += n;
                        if (esFilled + written >= esCap) return written;
                    } else {
                        c->pesSkip = esStart - plLen;
                    }
                }
            } else if (c->sawPusi) {
                const BYTE* q = pl; int n = plLen;
                if (c->pesSkip > 0) {
                    int skip = c->pesSkip > n ? n : c->pesSkip;
                    q += skip; n -= skip; c->pesSkip -= skip;
                }
                if (n > 0) {
                    int w = esAppend(c, q, n, esOut, esCap, esFilled + written);
                    written += w;
                    if (esFilled + written >= esCap) return written;
                }
            }
        }
    }
    return written;
}
