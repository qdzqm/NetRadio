#ifndef NETRADIO_RESOURCE_H
#define NETRADIO_RESOURCE_H

/* ---- Menu ---- */
#define IDM_MENU_RADIO          1000
#define IDM_FILE_RELOAD         1001
#define IDM_FILE_EXIT           1002
#define IDM_MENU_HELP           1010
#define IDM_HELP_ABOUT          1011

/* ---- Controls ---- */
#define IDC_STATION_LIST        1101
#define IDC_HDR_PLAYING         1102
#define IDC_LBL_NAME            1103
#define IDC_LBL_TITLE           1104
#define IDC_LBL_FORMAT          1105
#define IDC_LBL_URL             1106
#define IDC_LBL_STATUS          1107
#define IDC_PROGRESS_BUF        1108
#define IDC_TRACK_VOLUME        1109
#define IDC_LBL_VOL             1110
#define IDC_BTN_PREV            1111
#define IDC_BTN_PLAY            1112
#define IDC_BTN_PAUSE           1113
#define IDC_BTN_STOP            1114
#define IDC_BTN_NEXT            1115

/* Prefix labels (non-clickable) */
#define IDC_PRE_BUF             1120
#define IDC_PRE_VOL             1121

/* ---- Custom messages from worker thread to UI ---- */
#define WM_APP_RADIO_UPDATE     (WM_APP + 1)   /* lParam = RADIO_INFO* (heap, callee frees) */

/* ---- Timer ---- */
#define IDT_UI                  0x9001
#define IDT_MARQUEE             0x9002

/* ---- Accelerator base ---- */
#define IDA_SPACE               0xA001
#define IDA_S                   0xA002
#define IDA_M                   0xA003
#define IDA_PREV                0xA004
#define IDA_NEXT                0xA005
#define IDA_VOLUP               0xA006
#define IDA_VOLDN               0xA007
#define IDA_PLAYSEL             0xA008
#define IDA_STOP                0xA009
#define IDA_F5                  0xA00A

#endif
