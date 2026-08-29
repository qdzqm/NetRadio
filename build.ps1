# build.ps1 - 构建 FAAD2 静态库 + NetRadio.exe
# 优先使用 PATH 中的 gcc；找不到则自动搜索 WinGet 安装的 MinGW-w64
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    $mw = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*mingw64*" -Directory -ErrorAction SilentlyContinue |
          ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } |
          Where-Object { Test-Path (Join-Path $_ 'gcc.exe') } | Select-Object -First 1
    if (-not $mw) { Write-Output 'ERROR: gcc not found. Install MinGW-w64 or add it to PATH.'; exit 1 }
    $env:Path = "$mw;" + $env:Path
}
Set-Location $PSScriptRoot

$faadDefines = '-DHAVE_STDINT_H=1', '-DHAVE_STRING_H=1', '-DHAVE_LRINTF=1',
               '-DHAVE_COSF=1', '-DHAVE_LOGF=1', '-DHAVE_EXPF=1',
               '-DHAVE_FLOORF=1', '-DHAVE_CEILF=1', '-DHAVE_SQRTF=1',
               '-DPACKAGE_VERSION=\"2.11\"'

# 1. 编译 faad 所有 .c 为对象文件
New-Item -ItemType Directory -Force -Path faad\obj | Out-Null
$cs = Get-ChildItem faad\*.c
$ok = 0
foreach ($f in $cs) {
    $o = "faad\obj\" + $f.BaseName + ".o"
    & gcc -O2 -w @faadDefines -Ifaad -Ifaad\include -c $f.FullName -o $o 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { $ok++ } else { Write-Output "COMPILE FAIL: $($f.Name)"; & gcc -O2 -w @faadDefines -Ifaad -Ifaad\include -c $f.FullName -o $o; exit 1 }
}
Write-Output "faad objects OK: $ok/$($cs.Count)"

# 2. 打包静态库
if (Test-Path faad\libfaad.a) { Remove-Item faad\libfaad.a }
& ar rcs faad\libfaad.a faad\obj\*.o
Write-Output "libfaad.a = $((Get-Item faad\libfaad.a).Length) bytes"

# 3. 编译主程序
Remove-Item NetRadio.exe -ErrorAction SilentlyContinue
& gcc -Os -Wall -Wextra -DUNICODE -D_UNICODE -D_CRT_SECURE_NO_WARNINGS `
      -Isrc -Ifaad\include `
      src\main.c src\radio.c src\hls.c faad\libfaad.a `
      -o NetRadio.exe -municode -mwindows `
      -lcomctl32 -lshell32 -luser32 -lgdi32 -lwinmm -lwininet -lm
if (Test-Path NetRadio.exe) {
    Write-Output "NetRadio.exe OK $((Get-Item NetRadio.exe).Length) bytes"
} else {
    Write-Output "BUILD FAILED"
    exit 1
}
