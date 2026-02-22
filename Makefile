# ==============================================================================
# hajimu_gui — はじむ用 GUI パッケージ ビルドファイル
#
# 技術スタック: 自製プラットフォーム層 (hjp_platform + hjp_render) + OpenGL
#
# 使い方:
#   make               ビルド（hajimu_gui.hjp を生成）
#   make clean          ビルド成果物を削除
#   make install        ~/.hajimu/plugins/ にインストール
#   make uninstall      インストール済みプラグインを削除
#   make test           テストGUIアプリを起動
#   make help           ヘルプ表示
#
# クロスプラットフォーム対応（macOS / Linux / Windows MinGW）
# ==============================================================================

# プラグイン名
PLUGIN_NAME = hajimu_gui

# ソースファイル
SRC_C = src/hajimu_gui.c src/hjp_render.c \
	    src/hjp_vnode.c src/hjp_frame.c src/hjp_hotreload.c src/hjp_devtools.c

# はじむインクルードパス
HAJIMU_INCLUDE ?= $(shell \
	if [ -d "../../jp/include" ]; then echo "../../jp/include"; \
	elif [ -d "../jp/include" ]; then echo "../jp/include"; \
	elif [ -d "/usr/local/include/hajimu" ]; then echo "/usr/local/include/hajimu"; \
	else echo "include"; fi)

# コンパイラ
CC ?= cc

# 共通フラグ
CFLAGS = -Wall -Wextra -O2 -I$(HAJIMU_INCLUDE) -Isrc

# ==============================================================================
# プラットフォーム判定
# ==============================================================================

UNAME := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME),Darwin)
    # macOS — 自製プラットフォーム層 + OpenGL
    SHARED_FLAGS = -shared -dynamiclib -fPIC
    OUT = $(PLUGIN_NAME).hjp
    SRC_PLATFORM = src/hjp_platform_macos.m
    GL_LIBS = -framework OpenGL
    CFLAGS += -DGL_SILENCE_DEPRECATION
    LDFLAGS = $(GL_LIBS) -framework Cocoa -framework CoreText -framework CoreGraphics -framework ImageIO -lpthread
    INSTALL_DIR = $(HOME)/.hajimu/plugins
else ifeq ($(UNAME),Linux)
    # Linux — 自製プラットフォーム層 + OpenGL + X11 + FreeType
    SHARED_FLAGS = -shared -fPIC
    OUT = $(PLUGIN_NAME).hjp
    SRC_PLATFORM = src/hjp_platform_linux.c
    GL_LIBS = -lGL
    CFLAGS += $(shell pkg-config --cflags x11 freetype2 2>/dev/null)
    LDFLAGS = $(GL_LIBS) $(shell pkg-config --libs x11 freetype2 2>/dev/null) -ldl -lpthread -lm
    INSTALL_DIR = $(HOME)/.hajimu/plugins
else
    # Windows (MinGW)
    SHARED_FLAGS = -shared
    OUT = $(PLUGIN_NAME).hjp
    SRC_PLATFORM = src/hjp_platform_win32.c
    LDFLAGS = -lopengl32 -lgdi32 -luser32 -lshell32 -lkernel32 -lpthread
    INSTALL_DIR = $(USERPROFILE)\.hajimu\plugins
endif

# ==============================================================================
# フォントの埋め込みパス
# ==============================================================================

FONT_DIR = fonts
FONT_FILE = NotoSansCJKjp-Regular.otf

# ==============================================================================
# ターゲット
# ==============================================================================

.PHONY: all clean install uninstall test help

all: $(OUT)
	@echo ""
	@echo "  ✅ ビルド成功: $(OUT)"
	@echo ""
	@echo "  インストール:   make install"
	@echo "  テスト:         make test"
	@echo ""

$(OUT): $(SRC_C) $(SRC_PLATFORM)
	$(CC) $(SHARED_FLAGS) $(CFLAGS) -o $@ $(SRC_C) $(SRC_PLATFORM) $(LDFLAGS)

clean:
	rm -f $(OUT)
	@echo "  🧹 クリーン完了"

install: $(OUT)
	@mkdir -p $(INSTALL_DIR)/$(PLUGIN_NAME)
	@mkdir -p $(INSTALL_DIR)/$(PLUGIN_NAME)/fonts
	cp $(OUT) $(INSTALL_DIR)/$(PLUGIN_NAME)/
	cp hajimu.json $(INSTALL_DIR)/$(PLUGIN_NAME)/
	cp $(FONT_DIR)/$(FONT_FILE) $(INSTALL_DIR)/$(PLUGIN_NAME)/fonts/
	@echo ""
	@echo "  📦 インストール完了: $(INSTALL_DIR)/$(PLUGIN_NAME)/"
	@echo ""

uninstall:
	rm -rf $(INSTALL_DIR)/$(PLUGIN_NAME)
	@echo "  🗑  アンインストール完了"

# テスト起動
NIHONGO ?= $(shell \
	if [ -x "../../jp/nihongo" ]; then echo "../../jp/nihongo"; \
	elif command -v nihongo >/dev/null 2>&1; then echo "nihongo"; \
	else echo "./nihongo"; fi)

test: $(OUT)
	@echo "  🚀 テストGUIアプリを起動..."
	$(NIHONGO) examples/hello_gui.jp

help:
	@echo ""
	@echo "  hajimu_gui — はじむ用 GUI パッケージ"
	@echo ""
	@echo "  技術スタック: 自製プラットフォーム層 (hjp_platform + hjp_render) + OpenGL"
	@echo ""
	@echo "  ターゲット:"
	@echo "    make             ビルド ($(OUT))"
	@echo "    make clean       クリーン"
	@echo "    make install     ~/.hajimu/plugins/ にインストール"
	@echo "    make uninstall   アンインストール"
	@echo "    make test        テストGUI起動"
	@echo "    make help        このヘルプ"
	@echo ""
	@echo "  環境変数:"
	@echo "    HAJIMU_INCLUDE   はじむヘッダーパス (デフォルト: 自動検出)"
	@echo "    CC               コンパイラ (デフォルト: gcc)"
	@echo "    NIHONGO          はじむ実行パス (デフォルト: 自動検出)"
	@echo ""
