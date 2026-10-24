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

# Regenerates components/muse/fonts/muse_font_cjk_20.c, the chat view's
# larger CJK font (CONFIG_MUSE_CJK_FONT + CONFIG_MUSE_CHAT_VIEW): GNU
# Unifont's 20x20 bitmaps for just the GB2312 symbols and level-1 hanzi
# (~4,400 everyday glyphs, ~230 KB) -- a full-range 20 px font would not
# leave room in the app slot next to the 16 px one. The UI chains this
# font to muse_font_cjk_16, so rarer characters still render, smaller.
# Needs curl, python3 (for the GB2312 symbol list) and npx (Node.js).
set -eu

VERSION=16.0.04
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$HERE/components/muse/fonts/muse_font_cjk_20.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

SHA256=0e3981ab552231b5a2a870f2b61741903a4bf25c23ef5aeb05fdced1b3c7af4d
curl -fsSL -o "$TMP/unifont.otf" \
    "https://unifoundry.com/pub/unifont/unifont-$VERSION/font-builds/unifont-$VERSION.otf"
echo "$SHA256  $TMP/unifont.otf" | shasum -a 256 -c - >/dev/null
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
npx -y lv_font_conv@1.5.3 --font "$TMP/unifont.otf" --size 20 --bpp 1 \
    --format lvgl --lv-font-name muse_font_cjk_20 --no-compress \
    --symbols "$(cat "$TMP/symbols.txt")" -o "$TMP/font.c"
# The project includes LVGL as "lvgl.h"; the header names no temp paths.
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
