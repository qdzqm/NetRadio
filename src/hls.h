/* hls.h - m3u8 播放列表解析 + MPEG-TS 解封装 */
#ifndef HLS_H
#define HLS_H

#include <windows.h>

/* --- 一次性 HTTP GET（内部自管 WinInet 句柄） ---
 * 成功返回 1，*outBuf 需调用方 LocalFree；失败返回 0。
 * maxLen 为最大接收字节数（防内存爆炸）。
 * quit 为可选退出标志（非零时尽快中止，可为 NULL）。
 * outReq 为可选的请求句柄登记槽（volatile HINTERNET*）：函数在发起请求前
 * 把请求句柄写入 *outReq，返回前置 NULL。外部可通过 InternetCloseHandle 强制
 * 中断阻塞中的连接/读取（切台时用），可为 NULL。 */
int hlsHttpGet(const char* url, BYTE** outBuf, int* outLen, int maxLen,
               volatile LONG* quit, volatile HINTERNET* outReq);

/* --- 相对 URL 解析 --- */
void hlsResolveUrl(const char* baseUrl, const char* rel, char* out, int cap);

/* --- 播放列表解析结果 --- */
#define HLS_MAX_SEGS 64
typedef struct {
    int  nSegs;
    char segUrl[HLS_MAX_SEGS][768];
    int  mediaSeq;          /* 第一个分片对应的序号 */
    int  targetDur;         /* 秒 */
    int  endList;           /* EXT-X-ENDLIST 存在 */
    int  isMaster;          /* 主播放列表（含 STREAM-INF） */
    char variantUrl[768];   /* isMaster 时的第一个变体 URL */
} HlsPlaylist;

/* 解析 m3u8 文本；baseUrl 用于解析相对路径。成功返回 1。 */
int hlsParsePlaylist(const char* text, int textLen, const char* baseUrl,
                     HlsPlaylist* pl);

/* --- MPEG-TS 解封装 --- */
typedef struct {
    int pmtPid;
    int esPid;
    int esType;      /* 0=未知 0x0F/0x11=AAC 0x03/0x04=MP3 */
    int pesSkip;     /* PES 头部剩余待跳过字节 */
    int sawPusi;
} TsCtx;

void tsInit(TsCtx* c);

/* 喂入 TS 数据，把提取出的基本流(ES)字节追加到 esOut。
 * 返回写入 esOut 的字节数（≤ esCap - 已写入），缓冲满时停止并返回已写数量。
 * *pEsWithType 输出发现的第一条音频流类型。 */
int tsFeed(TsCtx* c, const BYTE* data, int len, BYTE* esOut, int esCap,
           int esFilled);

#endif /* HLS_H */
