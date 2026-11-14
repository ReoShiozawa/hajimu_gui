# 同梱版utf8proc

書記素境界の判定に使う。Unicode規則をGUI側で独自実装しないための依存であり、字形の合成や複雑な文字組みを行うエンジンではない。

- 上流: https://github.com/JuliaStrings/utf8proc
- バージョン: 2.12.0 / Unicode 18.0.0
- 固定コミット: `79cdd5ab40c79afa559f689ffc13b76812dee1ac`
- 取得元: https://github.com/JuliaStrings/utf8proc/archive/79cdd5ab40c79afa559f689ffc13b76812dee1ac.tar.gz
- アーカイブSHA-256: `e8fadfcd531d65525cbef157b866c5408e2e3f0d7c0f3aefb0bce01a7a08e35d`
- `utf8proc.c` / `utf8proc.h` / `utf8proc_data.c` / `LICENSE.md` は上流から無改変で収録
- `.gitattributes` で原本とUnicode公式テストの改行変換を無効化し、Windowsの自動CRLF変換でも固定ハッシュを維持する。ハッシュ一覧と検査スクリプトはLFで保持する
- ライセンス全文: [LICENSE.md](LICENSE.md)。バイナリの再配布にも同梱する

`src/hjp_unicode.c` から内部リンクし、追加のシステムライブラリは要求しない。macOS/Linuxではutf8procのシンボルを隠し、他のプラグインやホストが使う版との衝突を避ける。

更新時は固定コミットとチェックサムを更新し、そのUnicode版の公式GraphemeBreakTestとライセンスも収録する。`sh tests/check-grapheme.sh` とGUIの操作・描画回帰試験を実行する。チェックサムは取り込み後の変更検出用であり、脆弱性がないことを証明するものではない。
