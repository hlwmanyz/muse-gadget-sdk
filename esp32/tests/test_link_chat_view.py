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

    def test_session_takes_the_note_transcript_row(self):
        # A note turn's words only come back as a message.user row on the
        # subscription; the session backend must post them (once, matched
        # to this turn's ack id) or note turns show no user bubble.
        session = read("components/muse/muse_chat_session.cpp")
        self.assertIn('"message.user"', session)
        self.assertIn("muse_state_chat_post(MUSE_CHAT_USER, clean)", session)

    def test_chat_font_is_a_step_up_with_the_full_font_behind_it(self):
        ui = read("components/muse/muse_ui.c")
        self.assertIn("chat_font()", ui)
        # PuHuiTi 20, the typeface and size xiaozhi's chat uses.
        self.assertIn("muse_font_puhui_20", ui)
        self.assertIn("lv_obj_set_style_text_font(lbl, chat_font(), 0)", ui)
        cmake = read("components/muse/CMakeLists.txt")
        self.assertIn('CONFIG_MUSE_CJK_FONT AND CONFIG_MUSE_CHAT_VIEW', cmake)
        self.assertIn("fonts/muse_font_puhui_20.c", cmake)

    def test_no_bottom_caption_band_under_the_chat_view(self):
        # On small screens the caption is a dark band across the bottom;
        # with the chat view carrying the conversation it's one box too
        # many, so the band is created hidden.
        ui = read("components/muse/muse_ui.c")
        self.assertIn("lv_obj_add_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN)", ui)
        # And the per-frame caption refresh must not undo that hide while
        # the chat view is up: set_flag(HIDDEN, !caption[0]) resurrected
        # the band on the first caption of every turn (v21.4's miss).
        self.assertIn("if (lbl == s_caption_lbl && s_chat_visible)", ui)

    def test_chat_scrolls_by_measured_bottom_and_follows_speech(self):
        ui = read("components/muse/muse_ui.c")
        self.assertIn("lv_obj_get_scroll_bottom(s_chat_list)", ui)
        # The follow target only moves down: a question re-anchors at the
        # bottom, a reply at its top, and the spoken walk maps the
        # caption's progress through the reply onto the bubble's scroll
        # range. Counting caption lines used to park a long reply
        # mid-article: the caption splits on the caption band's width
        # while the bubble wraps on the chat font's.
        sync = ui[ui.index("static void chat_sync"):]
        sync = sync[:sync.index("static void chat_build")]
        self.assertIn("follow_y", sync)
        self.assertIn("anchor_y", sync)
        self.assertIn("muse_state_caption_progress()", sync)
        self.assertNotIn("follow_y += lv_font_get_line_height(chat_font()) + 4", sync)
        self.assertNotIn("scroll_to_view", sync)
        self.assertNotIn("* 3 / 4", sync)

    def test_board_frame_matches_the_glass(self):
        # The DIY panel's glass shows 240 of the controller's 320 rows;
        # a 320-tall frame draws its bottom quarter off the glass.
        board = read("components/muse/boards/board_muse_s3cam_diy.c")
        self.assertIn("#define LCD_H 240", board)
        # And the face fills the little square screen.
        self.assertIn(".avatar_px = 216", board)

    def test_note_bubble_marks_the_turn_with_its_duration(self):
        # The note transcript never returns to the device (the subscribe
        # stream sends no message.user row for note turns), so the user's
        # bubble marks the turn with the note's length when it is sent.
        session = read("components/muse/muse_chat_session.cpp")
        self.assertIn("语音 %d 秒", session)
        self.assertIn("muse_state_chat_post(MUSE_CHAT_USER, bubble)", session)

    def test_wake_endpoint_measures_loud_against_the_room(self):
        # An absolute loud line sits inside a living room's ambient band
        # and every note ran to the 15 s cap; the line is the recording's
        # own floor plus headroom, clamped.
        voice = read("components/muse/muse_voice.c")
        self.assertIn("st.floor_db + WAKE_LOUD_MARGIN_DB", voice)
        self.assertIn("WAKE_LOUD_MIN_DB", voice)
        self.assertIn("WAKE_LOUD_MAX_DB", voice)
        self.assertNotIn("> WAKE_LOUD_DB)", voice)

    def test_voice_turns_chain_without_the_wake_word(self):
        # A replied-to voice turn re-opens the mic (continuous
        # conversation); the follow-up listen gets a longer give-up, and
        # a silent one ends the chain through the speechless drop.
        voice = read("components/muse/muse_voice.c")
        self.assertIn("FOLLOWUP_GIVEUP_FRAMES", voice)
        self.assertIn("s_followup ? FOLLOWUP_GIVEUP_FRAMES : WAKE_GIVEUP_FRAMES", voice)
        self.assertIn("follow-up: listening again", voice)
        self.assertIn("s_replied = delivered", voice)

    def test_board_can_leave_the_mic_glyph_out(self):
        # build_button_icons honours a left-out talk_hint (the aux icon's
        # long-standing pattern), and this board leaves it out: on a
        # wake-word board the word is the button.
        ui = read("components/muse/muse_ui.c")
        self.assertIn("leaves talk_hint out", ui)
        board = read("components/muse/boards/board_muse_s3cam_diy.c")
        self.assertIn(".talk_hint = { LV_ALIGN_DEFAULT, 0, 0 }", board)

    def test_wake_turn_ends_two_and_a_half_seconds_after_speech(self):
        # 21.5's regression: the note ended one second after the word,
        # so the question never got recorded. A wake turn now records
        # normally and ends 2.5 s after the speaker stops -- a window
        # that covers both the beat after the word and the end of the
        # question.
        voice = read("components/muse/muse_voice.c")
        self.assertIn("WAKE_QUESTION_QUIET_FRAMES (MUSE_AUDIO_RATE * 5 / 2)", voice)
        self.assertIn("n - s_wake_last_loud >= WAKE_QUESTION_QUIET_FRAMES", voice)
        self.assertNotIn("WAKE_QUIET_FRAMES", voice)

    def test_followup_listen_drops_speech_that_never_becomes_a_question(self):
        # A follow-up listen is stricter than a wake turn: stray sounds
        # must grow into a real question or the listen is dropped quietly
        # at its give-up instead of sending noise on its own.
        voice = read("components/muse/muse_voice.c")
        self.assertIn("s_wake_confirmed", voice)
        self.assertIn("WAKE_WORD_SPAN_FRAMES", voice)
        self.assertIn("A follow-up listen that never grew into a question.", voice)

    def test_followup_confirmation_uses_sustained_average_not_peaks(self):
        # A single loud burst (a clap, a cough, one TV syllable) used to
        # confirm a follow-up on its own. Confirmation now needs the
        # sound so far to average well above the room's floor: real
        # speech near the mic averages ~25 dB above it, room noise that
        # only bursts above the line ~5 dB above it.
        voice = read("components/muse/muse_voice.c")
        self.assertIn("FOLLOWUP_AVG_MARGIN_DB 15.0f", voice)
        self.assertIn("FOLLOWUP_AVG_MIN_DB (-40.0f)", voice)
        self.assertIn("FOLLOWUP_AVG_MAX_DB (-30.0f)", voice)
        self.assertIn("st.floor_db + FOLLOWUP_AVG_MARGIN_DB", voice)
        self.assertIn("follow-up confirmed: avg %.1f dBFS, floor %.1f dBFS", voice)
        self.assertNotIn("WAKE_GAP_FRAMES", voice)
        # And the walk's progress reaches the UI through the state layer.
        session = read("components/muse/muse_chat_session.cpp")
        self.assertIn("muse_hatch_turn_caption_progress", session)
        self.assertIn("s_caption_progress", session)
        state = read("components/muse/muse_state.c")
        self.assertIn("muse_state_set_caption_progress", state)
        self.assertIn("muse_state_caption_progress", state)

    def test_version(self):
        self.assertIn('set(PROJECT_VER "21.10")', read("CMakeLists.txt"))


if __name__ == "__main__":
    unittest.main()
