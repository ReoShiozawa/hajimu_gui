# =============================================================================
# hajimu_gui — はじむ用 GUI パッケージ
# クロスプラットフォーム Makefile (macOS / Linux / Windows MinGW)
# =============================================================================

PLUGIN_NAME = hajimu_gui
SRC_C = src/hajimu_gui.c src/hjp_render.c \
        src/hjp_vnode.c src/hjp_frame.c src/hjp_hotreload.c src/hjp_devtools.c
OUT   = $(PLUGIN_NAME).hjp
CC   ?= gcc

# OS 判定 ($(OS) は Windows CMD/PowerShell で "Windows_NT" になる)
ifeq ($(OS),Windows_NT)
    DETECTED_OS := Windows
    INSTALL_DIR := $(USERPROFILE)/.hajimu/plugins
else
    DETECTED_OS := $(shell uname -s 2>/dev/null || echo Unknown)
    INSTALL_DIR := $(HOME)/.hajimu/plugins
endif

# はじむインクルードパス自動検出
ifeq ($(OS),Windows_NT)
    ifndef HAJIMU_INCLUDE
        HAJIMU_INCLUDE := $(or \
            $(if $(wildcard ../../jp/include/hajimu.h),../../jp/include),\
            $(if $(wildcard ../jp/include/hajimu.h),../jp/include),\
            ./include)
    endif
else
    ifndef HAJIMU_INCLUDE
        HAJIMU_INCLUDE := $(shell \
            if [ -d "../../jp/include" ]; then echo "../../jp/include"; \
            elif [ -d "../jp/include" ]; then echo "../jp/include"; \
            elif [ -d "/usr/local/include/hajimu" ]; then echo "/usr/local/include/hajimu"; \
            else echo "include"; fi)
    endif
endif

# プラットフォーム別フラグ
ifeq ($(OS),Windows_NT)
    SRC_PLATFORM = src/hjp_platform_win32.c
    CFLAGS  = -Wall -Wextra -O2 -I$(HAJIMU_INCLUDE) -Isrc
    CFLAGS += -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN
    CFLAGS += -shared
    LDFLAGS = -lopengl32 -lgdi32 -luser32 -lshell32 -lkernel32 -lpthread -static-libgcc
else ifeq ($(DETECTED_OS),Darwin)
    SRC_PLATFORM = src/hjp_platform_macos.m
    CFLAGS  = -Wall -Wextra -O2 -I$(HAJIMU_INCLUDE) -Isrc -fPIC -DGL_SILENCE_DEPRECATION
    CFLAGS += -shared -dynamiclib
    LDFLAGS = -framework OpenGL -framework Cocoa -framework CoreText -framework CoreGraphics -framework ImageIO -lpthread
else
    SRC_PLATFORM = src/hjp_platform_linux.c
    CFLAGS  = -Wall -Wextra -O2 -I$(HAJIMU_INCLUDE) -Isrc -fPIC
    CFLAGS += -shared
    CFLAGS += $(shell pkg-config --cflags x11 freetype2 2>/dev/null)
    LDFLAGS = -lGL $(shell pkg-config --libs x11 freetype2 2>/dev/null) -ldl -lpthread -lm
endif

FONT_DIR  = fonts
FONT_FILE = NotoSansCJKjp-Regular.otf

.PHONY: all clean install uninstall help

all: $(OUT)
	@echo "  ビルド完了: $(OUT)"

$(OUT): $(SRC_C) $(SRC_PLATFORM)
	$(CC) $(CFLAGS) -o $@ $(SRC_C) $(SRC_PLATFORM) $(LDFLAGS)

clean:
ifeq ($(OS),Windows_NT)
	-del /F /Q $(OUT) 2>NUL
else
	rm -f $(OUT)
endif
	@echo "  クリーン完了"

install: $(OUT)
ifeq ($(OS),Windows_NT)
	if not exist "$(INSTALL_DIR)\$(PLUGIN_NAME)" mkdir "$(INSTALL_DIR)\$(PLUGIN_NAME)"
	if not exist "$(INSTALL_DIR)\$(PLUGIN_NAME)\fonts" mkdir "$(INSTALL_DIR)\$(PLUGIN_NAME)\fonts"
	copy /Y $(OUT) "$(INSTALL_DIR)\$(PLUGIN_NAME)"
	copy /Y hajimu.json "$(INSTALL_DIR)\$(PLUGIN_NAME)"
	copy /Y $(FONT_DIR)\$(FONT_FILE) "$(INSTALL_DIR)\$(PLUGIN_NAME)\fonts"
else
	@mkdir -p $(INSTALL_DIR)/$(PLUGIN_NAME)/fonts
	cp $(OUT) $(INSTALL_DIR)/$(PLUGIN_NAME)/
	cp hajimu.json $(INSTALL_DIR)/$(PLUGIN_NAME)/
	cp $(FONT_DIR)/$(FONT_FILE) $(INSTALL_DIR)/$(PLUGIN_NAME)/fonts/
endif
	@echo "  インストール完了: $(INSTALL_DIR)/$(PLUGIN_NAME)/"

uninstall:
ifeq ($(OS),Windows_NT)
	-rmdir /S /Q "$(INSTALL_DIR)\$(PLUGIN_NAME)" 2>NUL
else
	rm -rf $(INSTALL_DIR)/$(PLUGIN_NAME)
endif
	@echo "  アンインストール完了"

help:
	@echo "  hajimu_gui — はじむ用 GUI パッケージ"
	@echo "  macOS:   (Cocoa/OpenGL は内蔵)"
	@echo "  Linux:   sudo apt install libx11-dev libfreetype6-dev libgl1-mesa-dev"
	@echo "  Windows: MSYS2 MinGW64 ターミナルで実行してください"
	@echo "    pacman -S mingw-w64-x86_64-gcc"
	@echo ""
