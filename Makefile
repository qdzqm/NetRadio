# =============================================================================
# NetRadio Makefile (MinGW-w64 GCC, POSIX sh / MSYS2 / Git-Bash)
#
#   make            构建 NetRadio.exe（首次运行自动检测/安装编译器）
#   make setup      重新检测（或自动下载安装）MinGW-w64 工具链
#   make check      显示当前使用的编译器
#   make clean      清除构建产物
#   make distclean  清除构建产物和生成的 config.mk
#
# 工具链检测逻辑见 tools/setup-toolchain.sh，检测结果写入 config.mk。
# =============================================================================

CONFIG := config.mk
-include $(CONFIG)

.PHONY: all setup check clean distclean

# 默认目标必须显式声明：否则第一个规则目标 config.mk 会成为默认目标
.DEFAULT_GOAL := all

# ---------------- 工具链 ----------------
# config.mk 由 tools/setup-toolchain.sh 生成，定义 CC / AR / BIN_DIR
$(CONFIG):
	@sh tools/setup-toolchain.sh

setup:
	@rm -f $(CONFIG)
	@$(MAKE) $(CONFIG)

check: $(CONFIG)
	@test -x "$(CC)" || { echo "ERROR: 编译器不可用: $(CC)，请运行 make setup"; exit 1; }
	@test -x "$(AR)"  || { echo "ERROR: ar 不可用: $(AR)，请运行 make setup"; exit 1; }
	@echo "CC = $(CC)"
	@"$(CC)" --version | head -n 1
	@"$(AR)" --version | head -n 1

# ---------------- 源文件 ----------------
FAAD_SRC := $(wildcard faad/*.c)
FAAD_OBJ := $(patsubst faad/%.c,faad/obj/%.o,$(FAAD_SRC))
FAAD_DEP := $(FAAD_OBJ:.o=.d)

APP_SRC  := src/main.c src/radio.c src/hls.c

# ---------------- 编译选项（与原 build.ps1 一致） ----------------
FAAD_CFLAGS := -O2 -w \
	-DHAVE_STDINT_H=1 -DHAVE_STRING_H=1 -DHAVE_LRINTF=1 \
	-DHAVE_COSF=1 -DHAVE_LOGF=1 -DHAVE_EXPF=1 \
	-DHAVE_FLOORF=1 -DHAVE_CEILF=1 -DHAVE_SQRTF=1 \
	-DPACKAGE_VERSION=\"2.11\" \
	-Ifaad -Ifaad/include

APP_CFLAGS := -Os -Wall -Wextra \
	-DUNICODE -D_UNICODE -D_CRT_SECURE_NO_WARNINGS \
	-Isrc -Ifaad/include

APP_LDFLAGS := -municode -mwindows
APP_LIBS    := -lcomctl32 -lshell32 -luser32 -lgdi32 -lwinmm -lwininet -lm

# ---------------- 目标 ----------------
all: check NetRadio.exe
	@echo "==> Build OK: NetRadio.exe"

faad/obj:
	@mkdir -p faad/obj

faad/obj/%.o: faad/%.c | $(CONFIG) faad/obj
	$(CC) $(FAAD_CFLAGS) -MMD -MP -c $< -o $@

faad/libfaad.a: $(FAAD_OBJ)
	$(AR) rcs $@ $^
	@echo "==> $@ built"

NetRadio.exe: $(APP_SRC) faad/libfaad.a | $(CONFIG)
	$(CC) $(APP_CFLAGS) $(APP_SRC) faad/libfaad.a -o $@ $(APP_LDFLAGS) $(APP_LIBS)

-include $(FAAD_DEP)

clean:
	rm -rf faad/obj faad/libfaad.a NetRadio.exe

distclean: clean
	rm -f $(CONFIG)
