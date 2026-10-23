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

"""camera.capture: advertised in link.register and dispatched in
on_ws_command() under the same option (CONFIG_MUSE_CAMERA, the promptless
roll-up of the per-board camera options), and each board with a camera
provides the muse_camera_* glue the dispatch calls."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
GATE = "#if CONFIG_MUSE_CAMERA"


class LinkCameraTest(unittest.TestCase):
    def test_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index(GATE)
        advertised = noise[start:noise.index("#endif", start)]
        self.assertIn('add_command(commands, "camera.capture"', advertised)
        self.assertIn('"timeout_ms", 30000', advertised)

        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        at = dispatch.index('"camera.capture"')
        dispatched = dispatch[dispatch.rindex(GATE, 0, at):dispatch.index("#endif", at)]
        self.assertIn("muse_camera_capture_task", dispatched)

    def test_preview_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index(GATE)
        advertised = noise[start:noise.index("#endif", start)]
        self.assertIn('add_command(commands, "camera.preview"', advertised)
        self.assertIn('"state"', advertised)

        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        at = dispatch.index('"camera.preview"')
        dispatched = dispatch[dispatch.rindex(GATE, 0, at):dispatch.index("#endif", at)]
        self.assertIn("muse_camera_preview_toggle()", dispatched)
        self.assertIn("muse_camera_preview_active()", dispatched)

    def test_capture_task_calls_the_glue_under_the_same_gate(self):
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static void muse_camera_capture_task(")
        block = app[app.rindex(GATE, 0, start):app.index("#endif", start)]
        self.assertIn("muse_camera_capture(&image, &error)", block)
        self.assertIn('"jpeg-base64"', block)

    def test_rollup_option_and_board_glue(self):
        kconfig = (ROOT / "components/muse/Kconfig").read_text()
        start = kconfig.index("config MUSE_CAMERA")
        rollup = kconfig[start:kconfig.index("\n    config ", start + 1)]
        self.assertIn("default y if MUSE_WATCHER_CAMERA || MUSE_S3CAM_CAMERA", rollup)
        for board in ("MUSE_WATCHER_CAMERA", "MUSE_S3CAM_CAMERA"):
            self.assertIn(f"config {board}", kconfig)
        boards = ROOT / "components/muse/boards"
        for glue in ("watcher_camera.c", "s3cam_camera.c"):
            source = (boards / glue).read_text()
            self.assertIn("bool muse_camera_capture(", source, glue)
            self.assertIn("void muse_camera_preview_toggle(", source, glue)
        header = (boards / "muse_camera.h").read_text()
        self.assertIn("bool muse_camera_capture(char **jpeg_base64, const char **error);", header)


if __name__ == "__main__":
    unittest.main()
