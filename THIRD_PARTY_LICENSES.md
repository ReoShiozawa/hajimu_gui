# サードパーティ ライセンス

hajimu_gui は以下のオープンソースソフトウェアを使用しています。

> **注記**: 描画エンジン (hjp_render.c)、フォント処理、画像読み込みはすべて自作実装です。
> nanovg、fontstash、stb_truetype、stb_image 等の外部ライブラリには依存していません。

---

## utf8proc

- 用途: Unicode書記素境界の判定
- バージョン: 2.12.0 / Unicode 18.0.0
- 上流: https://github.com/JuliaStrings/utf8proc
- ライセンス: MIT / 上流収録のUnicodeデータライセンス
- 全文: [vendor/utf8proc/LICENSE.md](vendor/utf8proc/LICENSE.md)
- 固定コミットと取り込み手順: [README.hajimu.md](vendor/utf8proc/README.hajimu.md)

実装とデータは無改変で同梱し、プラグインへ内部リンクする。インストール先では `UTF8PROC-LICENSE.md`、配布ビルドでは各バイナリに対応する `*.utf8proc-license.txt` を併せて配布する。

テスト用のUnicode公式GraphemeBreakTestは `tests/data/UNICODE-LICENSE.txt` の条件で収録する。

---

## Noto Sans CJK JP

- **著者**: Google
- **ライセンス**: SIL Open Font License, Version 1.1
- **URL**: https://fonts.google.com/noto/specimen/Noto+Sans+JP

```
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
Copyright 2012 Google Inc. All Rights Reserved.

This Font Software is licensed under the SIL Open Font License, Version 1.1.

This license is available with a FAQ at: http://scripts.sil.org/OFL

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The fonts and derivatives, however, cannot be released under any other
type of license. The requirement for fonts to remain under this license
does not apply to any document created using the fonts or their derivatives.
```

---
