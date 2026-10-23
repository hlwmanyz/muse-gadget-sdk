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

"""The S3CAM wake word: the board option, its promptless roll-up, the
esp-sr dependency, the model partition and the voice hooks all line up, so
the detector is compiled in exactly when the board asks for it."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
GATE = "#if CONFIG_MUSE_WAKEWORD"


class WakeWordTest(unittest.TestCase):
    def test_kconfig_options(self):
        kconfig = (ROOT / "components/muse/Kconfig").read_text()
        self.assertIn("config MUSE_S3CAM_WAKEWORD", kconfig)
        block = kconfig[kconfig.index("config MUSE_S3CAM_WAKEWORD"):]
        self.assertIn("depends on MUSE_BOARD_S3CAM_DIY && SPIRAM", block)
        self.assertIn("config MUSE_WAKEWORD", kconfig)
        self.assertIn("default y if MUSE_S3CAM_WAKEWORD", kconfig)

    def test_board_overlay_picks_the_model(self):
        overlay = (ROOT / "devices/sdkconfig.muse-s3cam-diy").read_text()
        self.assertIn("CONFIG_MUSE_S3CAM_WAKEWORD=y", overlay)
        self.assertIn("CONFIG_SR_WN_WN9_NIHAOXIAOXIN_TTS=y", overlay)

    def test_dependency_and_partition(self):
        deps = (ROOT / "components/muse/idf_component.yml").read_text()
        self.assertIn("espressif/esp-sr:", deps)
        self.assertIn('$CONFIG{MUSE_BOARD_ID} == \\"s3cam_diy\\"', deps)
        table = (ROOT / "partitions_muse.csv").read_text()
        self.assertIn("model,     data, spiffs,  0x830000, 1M,", table)
        # The model partition is appended: nothing before it moved.
        self.assertLess(table.index("prod_bak"), table.index("model,"))

    def test_voice_hooks_share_the_gate(self):
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        at = cmake.index("muse_wakenet.c")
        self.assertIn("if(CONFIG_MUSE_WAKEWORD)", cmake[:at])

        voice = (ROOT / "components/muse/muse_voice.c").read_text()
        self.assertIn('#include "muse_wakenet.h"', voice)
        feed = voice.index("muse_wakenet_feed(s_chunk, MUSE_AUDIO_CHUNK)")
        self.assertIn(GATE, voice[:feed])
        self.assertIn("s_wake_speechless", voice)
        # The model loads from muse_voice_start (the boot task, internal
        # stack): loading maps flash, which a PSRAM-stack task may not do.
        init = voice.index("muse_wakenet_init();")
        self.assertLess(voice.index("muse_voice_start(QueueHandle_t"), init)
        self.assertLess(init, voice.index("xTaskCreatePinnedToCoreWithCaps(voice_task"))
        header = (ROOT / "components/muse/muse_wakenet.h").read_text()
        self.assertIn("bool muse_wakenet_feed(", header)


if __name__ == "__main__":
    unittest.main()
