#ifndef NETRADIO_RADIO_H
#define NETRADIO_RADIO_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RS_IDLE = 0,       /* 未播放 */
    RS_CONNECTING,     /* 正在建立 HTTP 连接 */
    RS_BUFFERING,      /* 已连接，正在缓冲 */
    RS_PLAYING,        /* 正在播放 */
    RS_PAUSED,         /* 用户暂停 */
    RS_STOPPED,        /* 已停止 */
    RS_ERROR           /* 出错 */
} RADIO_STATE;

typedef struct {
    RADIO_STATE state;
    WCHAR  status[160];      /* 状态描述或错误信息（中文） */
    WCHAR  icyTitle[256];    /* 节目名（ICY StreamTitle） */
    WCHAR  fmtInfo[64];      /* 例如 "MP3 128 kbps 44.1 kHz Stereo" */
    WCHAR  url[512];         /* 当前 URL（显示用） */
    int    bufCur;           /* 当前缓冲毫秒 */
    int    bufMax;           /* 缓冲总容量毫秒 */
} RADIO_INFO;

/* 全局初始化/清理（InitCommonControls 之外，加载 winmm 用） */
void radio_startup(void);
void radio_cleanup(void);

/* 播放指定 URL；display 仅用于 UI 显示。返回 TRUE 表示工作线程已启动 */
BOOL radio_play(const char* utf8Url, const WCHAR* display);

/* 停止：立即中断读取、关闭 waveOut */
void radio_stop(void);

/* 暂停 / 恢复 */
void radio_set_paused(BOOL paused);
BOOL radio_is_paused(void);

/* 音量：0..100 */
void radio_set_volume(int pct);
int  radio_get_volume(void);

/* 供 UI 拉取当前快照 */
void radio_get_info(RADIO_INFO* out);

/* 当前是否正在播放任意 URL（含连接/缓冲/暂停） */
BOOL radio_is_active(void);

/* 预缓存指定电台：后台提前建连并缓冲数据，之后用同一 URL 调 radio_play
 * 可近乎即时开播。传其它 URL 会替换当前预缓存目标。 */
void radio_prewarm(const char* utf8Url);

#ifdef __cplusplus
}
#endif

#endif
