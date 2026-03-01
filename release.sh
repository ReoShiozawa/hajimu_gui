#!/bin/bash
# =============================================================================
# hajimu_gui — ローカルビルド + GitHub Release スクリプト
# =============================================================================
# 使い方:
#   ./release.sh              hajimu.json のバージョンでリリース
#   ./release.sh v13.0.2      指定バージョンでリリース
#   ./release.sh --push       ビルドなし、push + Release のみ
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")"

PLUGIN_NAME="hajimu_gui"
HJP_FILE="${PLUGIN_NAME}.hjp"

# ---------- 引数解析 ----------
VERSION=""
PUSH_ONLY=false

for arg in "$@"; do
  case "$arg" in
    --push)  PUSH_ONLY=true ;;
    v*|[0-9]*) VERSION="$arg" ;;
  esac
done

if [[ -z "$VERSION" ]]; then
  VERSION=$(jq -r '.バージョン // .version // empty' hajimu.json 2>/dev/null || true)
fi

if [[ -z "$VERSION" ]]; then
  echo "使い方: ./release.sh [バージョン] [--push]"
  exit 1
fi

[[ "$VERSION" == v* ]] || VERSION="v$VERSION"

echo "=== $PLUGIN_NAME $VERSION リリース ==="

if [[ "$PUSH_ONLY" == false ]]; then
  echo "--- 全プラットフォームビルド ---"
  make build-all
  echo "  → dist/"
fi

echo "--- Git push ---"
git add -A
git diff --cached --quiet || git commit -m "release: $PLUGIN_NAME $VERSION"
git push origin HEAD

if git rev-parse "$VERSION" >/dev/null 2>&1; then
  echo "  タグ $VERSION は既に存在します"
else
  git tag -a "$VERSION" -m "$PLUGIN_NAME $VERSION"
  echo "  タグ作成: $VERSION"
fi
git push origin "$VERSION"

if command -v gh >/dev/null 2>&1; then
  echo "--- GitHub Release 作成/更新 ---"
  ASSETS=(
    "dist/${PLUGIN_NAME}-macos.hjp"
    "dist/${PLUGIN_NAME}-linux-x64.hjp"
    "dist/${PLUGIN_NAME}-windows-x64.hjp"
  )

  # Release が存在しなければ作成、存在すれば upload --clobber で上書き
  if gh release view "$VERSION" >/dev/null 2>&1; then
    for f in "${ASSETS[@]}"; do
      gh release upload "$VERSION" "$f" --clobber
    done
  else
    gh release create "$VERSION" "${ASSETS[@]}" \
      --title "$PLUGIN_NAME $VERSION" \
      --generate-notes
  fi
fi

echo ""
echo "=== リリース完了: $PLUGIN_NAME $VERSION ==="
