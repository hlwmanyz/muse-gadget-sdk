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

"""The chat view (v21): the board option, the conversation log in
muse_state, both post points in the link turn, the UI's bubble view and
the draw-wakes-screen fix all line up, so a voice turn shows as WeChat-
style bubbles exactly on the boards that ask for it."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
GATE = "#if CONFIG_MUSE_CHAT_VIEW"


def read(rel):
    return (ROOT / rel).read_text()


class ChatViewTest(unittest.TestCase):
    def test_kconfig_options(self):
        kconfig = read("components/muse/Kconfig")
        self.assertIn("config MUSE_CHAT_VIEW", kconfig)
        block = kconfig[kconfig.index("config MUSE_CHAT_VIEW"):]
        self.assertIn("depends on MUSE_ENABLED", block)
        self.assertIn("config MUSE_CJK_FONT", kconfig)

    def test_board_overlay_enables_both(self):
        overlay = read("devices/sdkconfig.muse-s3cam-diy")
        self.assertIn("CONFIG_MUSE_CJK_FONT=y", overlay)
        self.assertIn("CONFIG_MUSE_CHAT_VIEW=y", overlay)

    def test_state_log_api(self):
        header = read("components/muse/muse_state.h")
        self.assertIn("muse_state_chat_post", header)
        self.assertIn("muse_state_chat_get", header)
        self.assertIn("muse_state_chat_count", header)
        self.assertIn("#define MUSE_CHAT_MAX 20", header)
        impl = read("components/muse/muse_state.c")
        self.assertIn("MALLOC_CAP_SPIRAM", impl)   # the ring lives in PSRAM
        self.assertIn("s_chat_seq % MUSE_CHAT_MAX", impl)
        # Eviction: a get too far behind the newest post fails.
        self.assertIn("s_chat_seq - seq <= MUSE_CHAT_MAX", impl)

    def test_link_posts_both_sides_under_the_gate(self):
        link = read("components/muse/muse_chat_link.c")
        self.assertGreaterEqual(link.count(GATE), 2)
        # The user's bubble is the whole transcript, not the caption tail.
        self.assertIn("muse_state_chat_post(MUSE_CHAT_USER, heard)", link)
        # The reply is posted whole, before end_turn clears the turn and
        # the DONE event reaches the voice task.
        post = link.index("muse_state_chat_post(MUSE_CHAT_ASSISTANT, s_turn.text)")
        done = link.index("MUSE_HATCH_EV_DONE", post)
        self.assertLess(post, done)

    def test_session_posts_both_sides_under_the_gate(self):
        # Boards with PSRAM (CONFIG_MUSE_HATCH) run the session backend,
        # not the link one: the posts must live there too, or the view
        # stays empty on exactly those boards.
        session = read("components/muse/muse_chat_session.cpp")
        self.assertGreaterEqual(session.count(GATE), 2)
        # The user's bubble is the final transcript, posted in post_chat
        # next to the HEARD event.
        post = session.index("muse_state_chat_post(MUSE_CHAT_USER, text)")
        heard = session.index("MUSE_HATCH_EV_HEARD, text", post - 200)
        self.assertLess(post, heard)
        # Each completed message posts its whole text as a bubble.
        self.assertIn("muse_state_chat_post(MUSE_CHAT_ASSISTANT, bubble)", session)
        # The C++ backend can only call the C state API through an
        # extern "C" header.
        self.assertIn('extern "C"', read("components/muse/muse_state.h"))

    def test_ui_view_and_draw_wake(self):
        ui = read("components/muse/muse_ui.c")
        self.assertIn(GATE, ui)
        self.assertIn("chat_build(scr)", ui)
        self.assertIn("chat_sync(mode, now)", ui)
        self.assertIn("0x95ec69", ui)   # the user's green bubble
        self.assertIn("MUSE_CHAT_MAX", ui)
        # A draw wakes the screen on entry, before the image buffer is
        # touched: asleep the present path never runs, and a redraw of the
        # image already up must still light the panel.
        draw = ui[ui.index("bool muse_ui_image_draw"):]
        self.assertLess(draw.index("if (muse_state_asleep())"),
                        draw.index("heap_caps_calloc"))

    def test_version(self):
        self.assertIn('set(PROJECT_VER "21.1")', read("CMakeLists.txt"))


if __name__ == "__main__":
    unittest.main()
