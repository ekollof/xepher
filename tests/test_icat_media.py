#!/usr/bin/env python3
"""Test video extraction and preview click targets without a running WeeChat."""

import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch


class IcatMedia(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            from PIL import Image
        except ImportError:
            raise unittest.SkipTest("Pillow is required")
        cls.Image = Image
        cls.ffmpeg = shutil.which("ffmpeg")
        if not cls.ffmpeg:
            raise unittest.SkipTest("ffmpeg is required")
        spec = importlib.util.spec_from_file_location(
            "xepher_icat_test", Path(__file__).resolve().parents[1] / "scripts/icat.py")
        cls.icat = importlib.util.module_from_spec(spec)
        cls.weechat = MagicMock()
        sys.modules[spec.name] = cls.icat
        with patch.dict(sys.modules, weechat=cls.weechat):
            spec.loader.exec_module(cls.icat)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="xepher media ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.icat.shared.cache_path = str(self.root / "cache")
        self.weechat.string_eval_path_home.side_effect = lambda path, *_: path
        self.video = self.root / "clip 'quoted', name.mp4"
        subprocess.run(
            [self.ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
             "color=c=red:s=80x48:d=0.1", "-c:v", "mpeg4", "-y", str(self.video)],
            check=True, capture_output=True, timeout=15)

    def test_frame_cache_and_original_click_target(self):
        frame = self.icat.preview_image_path(str(self.video))
        self.assertNotEqual(frame, str(self.video))
        with self.Image.open(frame) as image:
            self.assertEqual(image.size, (80, 48))
        with patch.object(self.icat.subprocess, "run", side_effect=AssertionError("cache miss")):
            self.assertEqual(self.icat.preview_image_path(str(self.video)), frame)
        placement = self.icat.ImagePlacement(str(self.video), 1, 8, 3)
        placement.cell_lines = ["preview"]
        self.icat.shared.graphics_backend = "cells"
        self.weechat.prnt_date_tags.reset_mock()
        self.icat.display_image("buffer", placement)
        tags = self.weechat.prnt_date_tags.call_args.args[2]
        self.assertIn("weeslack_file_" + os.fsencode(self.video.resolve()).hex(), tags)
        self.assertNotIn(os.fsencode(frame).hex(), tags)

    def test_extensionless_video_and_cache_invalidation(self):
        source = self.root / "encrypted-download"
        shutil.copyfile(self.video, source)
        first = self.icat.preview_image_path(str(source))
        info = source.stat()
        os.utime(source, ns=(info.st_atime_ns, info.st_mtime_ns + 1000000000))
        self.assertNotEqual(self.icat.preview_image_path(str(source)), first)

    def test_gif_stays_static_and_opens_original(self):
        path = self.root / "animated.gif"
        self.Image.new("RGB", (8, 8), "red").save(
            path, save_all=True, append_images=[self.Image.new("RGB", (8, 8), "blue")],
            duration=100, loop=0)
        with patch.object(self.icat.subprocess, "run", side_effect=AssertionError("unexpected ffmpeg")):
            self.assertEqual(self.icat.preview_image_path(str(path)), str(path))
            data = self.icat.load_image_data(str(path))
        with self.Image.open(io.BytesIO(data.data)) as image:
            self.assertEqual(image.convert("RGB").getpixel((0, 0)), (255, 0, 0))

    def test_missing_ffmpeg_and_failed_decode_leave_no_partial_frames(self):
        with patch.object(self.icat.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "require ffmpeg"):
                self.icat.preview_image_path(str(self.video))
        source = self.root / "invalid.mp4"
        source.touch()
        with self.assertRaises(RuntimeError):
            self.icat.preview_image_path(str(source))
        with patch.object(self.icat.subprocess, "run",
                          side_effect=subprocess.TimeoutExpired("ffmpeg", 20)):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.icat.preview_image_path(str(self.video))
        self.assertEqual(list((self.root / "cache/video_frames").iterdir()), [])

    def test_cells_render_frame_but_keep_video_path(self):
        placement = self.icat.ImagePlacement(str(self.video), 1, 8, 3)
        with patch.object(self.icat, "detect_graphics_backend", return_value="cells"), \
                patch.object(self.icat, "encode_cells", return_value=["preview"]) as encoder:
            self.icat.send_image_to_terminal(placement, terminal_size=MagicMock(), defer_tty_write=True)
        self.assertTrue(encoder.call_args.args[0].endswith(".png"))
        self.assertEqual(placement.path, str(self.video))

    def test_sixel_and_kitty_render_static_frame(self):
        placement = self.icat.ImagePlacement(str(self.video), 1, 8, 3)
        with patch.object(self.icat, "detect_graphics_backend", return_value="sixel"), \
                patch.object(self.icat, "_env", return_value="sixel"), \
                patch.object(self.icat, "encode_sixel", return_value=b"sixel") as encoder:
            self.icat.send_image_to_terminal(placement, terminal_size=MagicMock(), defer_tty_write=True)
        self.assertTrue(encoder.call_args.args[0].endswith(".png"))
        self.assertEqual(placement.path, str(self.video))
        placement = self.icat.ImagePlacement(str(self.video), 1, 8, 3)
        with patch.object(self.icat, "detect_graphics_backend", return_value="kitty"), \
                patch.object(self.icat, "write_chunked", return_value=[b"kitty"]) as encoder:
            self.icat.send_image_to_terminal(placement, defer_tty_write=True)
        with self.Image.open(io.BytesIO(encoder.call_args.args[1])) as image:
            self.assertEqual(image.format, "PNG")

    def test_cells_video_is_queued_instead_of_decoded_on_ui_thread(self):
        with patch.object(self.icat, "detect_graphics_backend", return_value="cells"), \
                patch.object(self.icat, "create_and_send_image_to_terminal") as queued, \
                patch.object(self.icat.subprocess, "run", side_effect=AssertionError("UI decode")):
            self.icat.create_image("buffer", str(self.video), 8, 3, True)
        queued.assert_called_once()


if __name__ == "__main__":
    unittest.main()
