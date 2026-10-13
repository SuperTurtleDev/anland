#!/usr/bin/env python3
"""Check camera identities and the Niri package layout without root or devices."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
CONFIG = ROOT / "libdisplay_producer/wireplumber/50-anland-android-camera.conf"


class AndroidCameraRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Use PipeWire's parser, including its relaxed JSON syntax.
        cls.rules = json.loads(subprocess.check_output(
            ["spa-json-dump", str(CONFIG)], text=True))["monitor.v4l2.rules"]
        cls.assert_config = cls.rules[0]
        pattern = cls.assert_config["matches"][0]["device.sysfs.path"]
        assert pattern.startswith("~")
        cls.pattern = re.compile(pattern[1:])

    def test_camera_control_nodes_and_renumbering(self):
        for driver in ("soc:qcom,cam-req-mgr", "soc:qcom,cam-sync",
                       "aa00000.qcom,vidc"):
            for number in (0, 1, 32, 33, 108):
                with self.subTest(driver=driver, number=number):
                    path = f"/devices/platform/soc/{driver}/video4linux/video{number}"
                    self.assertRegex(path, self.pattern)
        self.assertTrue(self.assert_config["actions"]["update-props"]["device.disabled"])

    def test_regular_camera_sources_are_preserved(self):
        paths = [
            "/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/video4linux/video0",
            "/devices/platform/soc/xhci-hcd/usb1/1-2/video4linux/video33",
            "/devices/virtual/video4linux/video0",  # v4l2loopback
            "/devices/platform/soc/ac00000.qcom,camss/video4linux/video0",
            "/devices/platform/soc/not-qcom,camera/video4linux/video1",
            "/devices/platform/soc/soc:qcom,cam-req-mgr/sound/card0",
            "",  # Anland's client-created PipeWire camera has no udev sysfs path.
        ]
        for path in paths:
            with self.subTest(path=path):
                self.assertIsNone(self.pattern.fullmatch(path))

    def test_does_not_disable_whole_video_monitor(self):
        self.assertNotIn("wireplumber.profiles", json.loads(subprocess.check_output(
            ["spa-json-dump", str(CONFIG)], text=True)))

    def test_niri_package_owns_config(self):
        with tempfile.TemporaryDirectory(prefix="anland-package-") as tmp:
            stage = Path(tmp)
            src = stage / "src"
            pkg = stage / "pkg"
            fixtures = {
                "cargo-target/release/niri": "binary fixture",
                "bridge-build/libanland_niri_bridge.so": "bridge fixture",
                "bridge-build/common/libdisplay_producer/libdisplay_producer.so": "library fixture",
                "niri-26.04/resources/default-config.kdl": "configuration fixture",
                "niri-26.04/LICENSE": "license fixture",
            }
            for relative, text in fixtures.items():
                path = src / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            rule = src / "anland-overlay/libdisplay_producer/wireplumber" / CONFIG.name
            rule.parent.mkdir(parents=True)
            shutil.copyfile(CONFIG, rule)
            subprocess.run([
                "bash", "-euc", 'source "$1"; srcdir="$2"; pkgdir="$3"; package',
                "package-test", str(ROOT / "producers/niri/Arch_v5/PKGBUILD"),
                str(src), str(pkg),
            ], check=True)
            installed = pkg / "usr/share/wireplumber/wireplumber.conf.d/50-anland-niri-android-camera.conf"
            self.assertEqual(installed.read_bytes(), CONFIG.read_bytes())
            self.assertEqual(installed.stat().st_mode & 0o777, 0o644)
            self.assertFalse((pkg / "etc").exists())


if __name__ == "__main__":
    unittest.main()
