# NetRadio — 不到 1 MB 的网络收音机 / A Sub-1MB Internet Radio Player

![size](https://img.shields.io/badge/exe%20size-421%20KB-brightgreen)
![deps](https://img.shields.io/badge/runtime%20dependencies-none-blue)
![platform](https://img.shields.io/badge/platform-Windows%207%2B-lightgrey)

## ✨ 有多小？/ How small is it?

**整个软件只有一个可执行文件：`NetRadio.exe`，仅 421 KB——不到 1 MB。**
没有安装包、没有运行时依赖、没有 DLL、没有配置文件数据库。下载一个文件，双击就能听广播。

**The entire program is a single executable: `NetRadio.exe`, only 421 KB — less than 1 MB.**
No installer, no runtime dependencies, no extra DLLs. Download one file, double-click, and listen.

---

## 中文说明

### 功能特性

- **小巧**：单文件 421 KB，纯 Win32 API 编写，无任何第三方运行时
- **格式全**：支持 MP3 / AAC / AAC+ (HE-AAC) 直播流，以及 HLS (m3u8) 直播
- **稳定**：1 MB 压缩环形缓冲吸收网络抖动，自动重连、欠载自动重新蓄缓冲，直播模式自动丢弃旧数据贴近直播边缘
- **秒切台**：后台预缓存下一个电台，切台几乎无等待
- **歌曲信息**：自动解析 ICY 元数据显示"歌手 - 歌名"，支持 UTF-8 / GBK / Big5 及双重编码乱码修复，超长文本走马灯滚动
- **界面**：电台列表 + 播放控制 + 音量 + 信息条（频道/节目/格式/地址/状态/缓冲进度）

### 使用方法

1. 运行 `NetRadio.exe`
2. 电台列表从同目录的 `stations.txt` 读取，每行格式：`显示名=URL`
3. 双击电台即可播放；支持 `#` 注释行

### 从源码构建

需要 MinGW-w64 GCC（Windows），然后执行：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

脚本会先编译内置的 FAAD2 源码为静态库，再链接生成 `NetRadio.exe`。

### 技术实现

| 部分 | 方案 |
|---|---|
| 界面 | 纯 Win32 GDI，双缓冲自绘，走马灯 |
| 网络 | WinINet（HTTP/HTTPS），ICY 元数据状态机 |
| 解码 | miniMP3（MP3）+ FAAD2（AAC/HE-AAC），静态链接 |
| 音频 | waveOut 事件回调，多级缓冲 |
| HLS | 自实现播放列表解析 + TS 解封装 + 分片预取 |

## English

### Features

- **Tiny**: a single 421 KB executable written in pure Win32 API, zero third-party runtime
- **Formats**: MP3 / AAC / AAC+ (HE-AAC) live streams and HLS (m3u8) live streams
- **Stable**: 1 MB compressed ring buffer absorbs network jitter; auto reconnect, auto re-buffer on underrun, live mode drops stale data to stay close to the live edge
- **Instant switching**: the next station is pre-buffered in the background
- **Song metadata**: parses ICY metadata to show "Artist - Title"; handles UTF-8 / GBK / Big5 and even fixes double-encoded mojibake; long texts scroll like a marquee
- **UI**: station list, playback controls, volume slider, and an info bar (channel / program / format / URL / status / buffering progress)

### Usage

1. Run `NetRadio.exe`
2. Stations are loaded from `stations.txt` in the same directory, one `Name=URL` per line
3. Double-click a station to play; lines starting with `#` are comments

### Building from source

Requires MinGW-w64 GCC on Windows, then run:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

The script compiles the bundled FAAD2 sources into a static library and links `NetRadio.exe`.

### How it works

| Part | Approach |
|---|---|
| UI | Pure Win32 GDI, double-buffered custom drawing, marquee labels |
| Network | WinINet (HTTP/HTTPS) with an ICY metadata state machine |
| Decoding | miniMP3 (MP3) + FAAD2 (AAC/HE-AAC), statically linked |
| Audio | waveOut with event callbacks and multi-stage buffering |
| HLS | Custom playlist parser + TS demuxer + segment prefetch |

---

## 许可 / License

- NetRadio 本体源码可自由使用 / The NetRadio source code is free to use
- [FAAD2](https://sourceforge.net/projects/faac/) 遵循其原有许可（包含于 `faad/` 目录）/ FAAD2 keeps its original license (sources in `faad/`)
- [miniMP3](https://github.com/lieff/minimp3) 为公有领域 / miniMP3 is public domain
- `stations.txt` 中的电台流地址来自公开渠道，仅供个人测试 / Station URLs in `stations.txt` come from public sources, for personal testing only
