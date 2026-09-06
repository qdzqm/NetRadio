#!/bin/sh
# =============================================================================
# tools/setup-toolchain.sh
#
# 检测可用的 MinGW-w64 GCC：
#   1. PATH 中的 gcc（实际试编译验证，不只是看是否存在）
#   2. WinGet 安装的 WinLibs / MinGW 包目录
#   3. 常见安装位置：C:\mingw64、C:\msys64\mingw64、C:\MinGW 等
#   4. 项目本地 ./toolchain/mingw64（本脚本自动下载的便携版）
#
# 若全部找不到：
#   a. 尝试 winget 自动安装 WinLibs (POSIX/UCRT)；
#   b. winget 不可用则用 curl 从 GitHub 下载 WinLibs 便携版 zip，
#      解压到项目内 toolchain/（无需管理员权限，不写绝对路径到任何文件）。
#
# 成功后把 CC / AR 写入 config.mk 供 Makefile 使用。
# =============================================================================

set -u

PROJDIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJDIR" || exit 1

CFG="config.mk"
TCDIR="$PROJDIR/toolchain"
WINLIB_ID="BrechtSanders.WinLibs.POSIX.UCRT"

log() { printf '%s\n' "$*"; }

# C:\foo\bar -> /c/foo/bar （供 sh 通配符使用）
msys_path() {
  echo "$1" | sed 's|\\|/|g; s|^\([A-Za-z]\):|/\L\1|'
}

# /c/foo/bar -> C:/foo/bar （供原生 Windows 程序 / make 使用）
native_path() {
  echo "$1" | sed 's|^/\([a-zA-Z]\)/|\1:/|'
}

# 试编译+链接一个最小程序，确认 gcc 真正可用
# （用临时文件而非 stdin 管道——原生 gcc 在 MSYS 管道下会挂起等待）
test_gcc() {
  g="$1"
  [ -n "$g" ] || return 1
  [ -f "$g" ] || return 1
  tmp="$(mktemp -d 2>/dev/null)" || tmp="$PROJDIR/._tc_tmp_$$"
  mkdir -p "$tmp" 2>/dev/null || return 1
  printf 'int main(void){return 0;}\n' > "$tmp/t.c"
  "$g" "$tmp/t.c" -o "$tmp/t.exe" >/dev/null 2>&1
  rc=$?
  rm -rf "$tmp" 2>/dev/null
  return $rc
}

FOUND_BIN=""
find_gcc() {
  FOUND_BIN=""

  # 1) PATH 中的 gcc
  if gp=$(command -v gcc 2>/dev/null) && [ -n "$gp" ] && test_gcc "$gp"; then
    FOUND_BIN="$(dirname "$gp")"
    log "  [found] PATH: $gp"
    return 0
  fi

  # 2) WinGet 包目录（%LOCALAPPDATA%\Microsoft\WinGet\Packages\*\mingw64\bin）
  LAD=""
  [ -n "${LOCALAPPDATA:-}" ] && LAD="$(msys_path "$LOCALAPPDATA")"
  if [ -n "$LAD" ]; then
    for g in "$LAD"/Microsoft/WinGet/Packages/*/mingw64/bin/gcc.exe; do
      [ -e "$g" ] || continue
      if test_gcc "$g"; then FOUND_BIN="$(dirname "$g")"; log "  [found] WinGet: $g"; return 0; fi
    done
  fi

  # 3) 常见安装位置
  for g in /c/mingw64/bin/gcc.exe \
           /c/msys64/mingw64/bin/gcc.exe \
           /c/MinGW/bin/gcc.exe \
           /mingw64/bin/gcc.exe; do
    [ -e "$g" ] || continue
    if test_gcc "$g"; then FOUND_BIN="$(dirname "$g")"; log "  [found] $g"; return 0; fi
  done

  # 4) 项目内便携工具链
  if [ -e "$TCDIR/mingw64/bin/gcc.exe" ] && test_gcc "$TCDIR/mingw64/bin/gcc.exe"; then
    FOUND_BIN="$TCDIR/mingw64/bin"
    log "  [found] project-local: $TCDIR/mingw64/bin/gcc.exe"
    return 0
  fi

  return 1
}

write_config() {
  bin="$(native_path "$1")"
  {
    echo "# 由 tools/setup-toolchain.sh 自动生成，请勿手动编辑"
    echo "BIN_DIR := $bin"
    echo "CC := \$(BIN_DIR)/gcc.exe"
    echo "AR := \$(BIN_DIR)/ar.exe"
    echo "export PATH := \$(BIN_DIR):\$(PATH)"
  } > "$CFG"
  log "==> 工具链就绪: $bin/gcc.exe"
  log "==> 已写入 $CFG"
}

# --- 安装方式 a：winget ----------------------------------------------------
install_winget() {
  command -v winget >/dev/null 2>&1 || { log "  winget 不可用"; return 1; }
  log "==> 通过 winget 安装 $WINLIB_ID ..."
  winget install --id "$WINLIB_ID" -e \
    --accept-source-agreements --accept-package-agreements --silent \
    || { log "  winget 安装失败"; return 1; }
  return 0
}

# --- 安装方式 b：下载 WinLibs 便携版 ---------------------------------------
install_download() {
  # 选择 curl：优先 PATH，再找 System32 自带
  CURL=""
  sysroot="$(msys_path "${SYSTEMROOT:-C:\\Windows}")"
  for c in curl "$sysroot/System32/curl.exe"; do
    if command -v "$c" >/dev/null 2>&1; then CURL="$c"; break; fi
  done
  [ -n "$CURL" ] || { log "  未找到 curl，无法下载"; return 1; }

  # Windows 10+ 自带的 bsdtar（System32\tar.exe）可直接解压 zip
  TAR=""
  for t in "$sysroot/System32/tar.exe" tar; do
    if command -v "$t" >/dev/null 2>&1; then TAR="$t"; break; fi
  done

  log "==> 查询 WinLibs 最新版本 (GitHub) ..."
  api="$("$CURL" -fsSL https://api.github.com/repos/brechtsanders/winlibs_mingw/releases/latest 2>/dev/null)" \
    || { log "  无法访问 GitHub API"; return 1; }

  url=$(printf '%s' "$api" \
        | grep -o 'https://[^"]*\.zip' \
        | grep 'x86_64-posix-seh' \
        | grep -i 'ucrt' \
        | head -n1)
  [ -n "$url" ] || { log "  未在 release 中找到 x86_64 posix ucrt zip"; return 1; }
  log "  下载: $url"

  mkdir -p "$TCDIR" || return 1
  "$CURL" -fL "$url" -o "$TCDIR/winlibs.zip" || { log "  下载失败"; return 1; }

  log "  解压到 toolchain/ ..."
  if [ -n "$TAR" ]; then
    "$TAR" -xf "$TCDIR/winlibs.zip" -C "$TCDIR" || { log "  解压失败"; return 1; }
  else
    log "  未找到可解压 zip 的 tar，请手动解压 $TCDIR/winlibs.zip 到 $TCDIR"
    return 1
  fi
  rm -f "$TCDIR/winlibs.zip"
  [ -e "$TCDIR/mingw64/bin/gcc.exe" ] || { log "  解压后未发现 mingw64/bin/gcc.exe"; return 1; }
  return 0
}

# =============================================================================
log "== 检测 MinGW-w64 GCC 工具链 =="
if find_gcc; then
  write_config "$FOUND_BIN"
  exit 0
fi

log "未找到可用的 GCC，尝试自动安装..."

if install_winget; then
  log "==> 重新检测工具链 ..."
  if find_gcc; then write_config "$FOUND_BIN"; exit 0; fi
fi

log "winget 方式失败，尝试下载便携版 WinLibs ..."
if install_download; then
  log "==> 重新检测工具链 ..."
  if find_gcc; then write_config "$FOUND_BIN"; exit 0; fi
fi

log ""
log "ERROR: 无法自动获取 MinGW-w64 工具链。"
log "请手动安装 MinGW-w64 (https://winlibs.com/) 并将其 bin 目录加入 PATH，"
log "或解压到项目内 toolchain/mingw64 后重新运行 make。"
exit 1
