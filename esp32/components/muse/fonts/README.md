# Fonts

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
`tools/muse/gen_cjk_font.sh` regenerates it.

`muse_font_cjk_20.c` is the chat view's larger CJK font, built in only with
`CONFIG_MUSE_CJK_FONT` and `CONFIG_MUSE_CHAT_VIEW`: the same Unifont at
20 px, but just the GB2312 symbols and level-1 hanzi (about 4,400 glyphs,
~230 KB), because a full-range 20 px font would crowd the app slot. The
chat font chains it to the 16 px font, so a rarer character still renders,
just smaller. `tools/muse/gen_cjk_font_20.sh` regenerates it.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file is a conversion of an unaltered subset of the font.
