# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
`tools/muse/gen_cjk_font.sh` regenerates it.

`muse_font_puhui_20.c` is the chat view's font, built in only with
`CONFIG_MUSE_CJK_FONT` and `CONFIG_MUSE_CHAT_VIEW`: Alibaba PuHuiTi 3.0
Regular at 20 px / 2 bpp -- the typeface and size xiaozhi's chat uses
(78/xiaozhi-fonts), with even strokes instead of a bitmap font stretched
past its cell. It covers ASCII plus the GB2312 symbols and level-1 hanzi
(about 4,500 glyphs, ~370 KB); a full-range font at this size would crowd
the app slot, and the chat font chains it to the 16 px font above, so a
rarer character still renders, just smaller.
`tools/muse/gen_puhui_font_20.sh` regenerates it. PuHuiTi is Alibaba's
typeface, free for commercial use; xiaozhi uses it at 4 bpp from its own
flash area, the app slot here leaves room for 2 bpp.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file is a conversion of an unaltered subset of the font.

`muse_font_emoji_16.c` is the caption font's colour emoji, built in only with
`CONFIG_MUSE_EMOJI`: 190 of the emoji most common in chat replies, as 16x16
RGB565A8 images, the height of unscii-16 and two of its cells wide, about
150 KB of flash. `muse_font_emoji_16` is an LVGL font whose glyphs are those
images. The caption font falls back to it last, and `muse_text.c` keeps in
captions only the emoji in `muse_emoji.h`, its list, so the others still go
rather than show as boxes. To change the set, edit `EMOJI` in
`tools/muse/gen_emoji_font.py` and run it with Pillow installed:
`python3 tools/muse/gen_emoji_font.py`. It downloads the pinned Twemoji
release, checks its SHA-256 and writes both files; pass a checkout of the
release instead to work offline.

The emoji are Twemoji v16.0.1's 72x72 graphics (https://github.com/jdecked/twemoji),
copyright Twitter, Inc and other contributors, and Jason Sofonia, Justine De
Caires and other contributors, licensed under CC-BY 4.0
(https://creativecommons.org/licenses/by/4.0/; the full text is in
`LICENSE-twemoji.txt`). They are changed: scaled to 16x16 and converted to
RGB565A8.
