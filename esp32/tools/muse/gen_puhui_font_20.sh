#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Regenerates components/muse/fonts/muse_font_puhui_20.c, the chat view's
# font (CONFIG_MUSE_CJK_FONT + CONFIG_MUSE_CHAT_VIEW): Alibaba PuHuiTi 3.0
# Regular at 20 px / 2 bpp -- the typeface and size xiaozhi's chat uses
# (78/xiaozhi-fonts), whose even, anti-aliased strokes read far better
# than a bitmap font stretched to 20 px. Only ASCII plus the GB2312
# symbols and level-1 hanzi are converted (~4,500 glyphs, ~370 KB): a
# full-range font at this size would crowd the app slot, and the UI
# chains this font to muse_font_cjk_16 for anything rarer. Xiaozhi
# itself uses 4 bpp with the font in its own flash area; the app slot
# here leaves room for 2. Needs curl, python3 and npx (Node.js).
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_puhui_20.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SHA256=be33bc8d45ced30fa5c542f6f056565baf85003d4e079522110d76016f8a04e2
curl -fsSL -o "$TMP/puhui.ttf" \
    "https://unpkg.com/@icraft/engine/public/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"
echo "$SHA256  $TMP/puhui.ttf" | shasum -a 256 -c - >/dev/null
# GB2312 rows 1-9 (symbols) and 16-55 (level-1 hanzi), as Unicode chars.
python3 - "$TMP/symbols.txt" <<'PYEOF'
import sys
chars = []
for q in list(range(1, 10)) + list(range(16, 56)):
    for w in range(1, 95):
        try:
            ch = bytes([q + 0xA0, w + 0xA0]).decode('gb2312')
        except UnicodeDecodeError:
            continue
        if len(ch) == 1:
            chars.append(ch)
seen = set()
out = []
for c in chars:
    if c not in seen and not c.isspace():
        seen.add(c)
        out.append(c)
open(sys.argv[1], 'w', encoding='utf-8').write(''.join(out))
PYEOF
npx -y lv_font_conv@1.5.3 --font "$TMP/puhui.ttf" --size 20 --bpp 2 \
    --format lvgl --lv-font-name muse_font_puhui_20 --no-compress \
    --symbols "$(cat "$TMP/symbols.txt")" -r 0x20-0x7E -o "$TMP/font.c"
# The project includes LVGL as "lvgl.h"; the header names no temp paths.
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
