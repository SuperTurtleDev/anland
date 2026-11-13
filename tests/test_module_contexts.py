#!/usr/bin/env python3
"""Exercise the real installer/boot scripts in private filesystem fixtures.

Only fixed Android paths are redirected. Production policy remains in the
scripts under test. Run with python3 -m unittest discover -s tests -v.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STOCK = "activity u:object_r:activity_service:s0\n* u:object_r:default_android_service:s0\n"
MAPPING = "anland.host                              u:object_r:anland_host_service:s0\n"


class ModuleContextsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="anland-context-test-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.module = self.base / "anland-awl"
        shutil.copytree(ROOT / "module", self.module)
        self.source = self.base / "platform-contexts"
        self.source.write_text(STOCK)
        self.log = self.base / "boot.log"
        self.overlay = self.module / "system/etc/selinux/plat_service_contexts"
        self.hash_file = self.module / "plat_service_contexts.source.sha256"
        for filename in ["customize.sh", "post-fs-data.sh"]:
            path = self.module / filename
            source = path.read_text()
            self.assertEqual(source.count("=/system/etc/selinux/plat_service_contexts"), 1)
            source = source.replace("=/system/etc/selinux/plat_service_contexts", "=" + str(self.source))
            source = source.replace("LOG=/data/local/tmp/awl_post_fs_data.log", "LOG=" + str(self.log))
            path.write_text(source)

    def install(self, check=True):
        # KernelSU's permission/abort API, with all writes scoped to the fixture.
        wrapper = '''
set_perm() { chmod "$4" "$1"; }
set_perm_recursive() { :; }
abort() { echo "$*" >&2; exit 1; }
export MODPATH="$1"
. "$MODPATH/customize.sh"
'''
        (self.module / "waylandbridge").touch()
        return subprocess.run(["sh", "-c", wrapper, "installer", str(self.module)],
                              capture_output=True, text=True, check=check)

    def boot(self):
        return subprocess.run(["sh", str(self.module / "post-fs-data.sh")],
                              capture_output=True, text=True, check=True)

    def assert_disabled(self):
        self.boot()
        self.assertTrue((self.module / "disable").exists())
        self.assertTrue((self.module / "skip_mount").exists())
        self.assertIn("reinstall", self.log.read_text())

    def test_install_inserts_before_wildcard_and_hashes_stock(self):
        self.install()
        self.assertEqual(self.overlay.read_text(), STOCK.replace("* ", MAPPING + "* "))
        self.assertEqual(self.hash_file.read_text().strip(), hashlib.sha256(STOCK.encode()).hexdigest())
        self.assertEqual((self.module / "post-fs-data.sh").stat().st_mode & 0o777, 0o755)

    def test_unchanged_rom_remains_enabled_and_clears_old_log(self):
        self.install()
        self.log.write_text("old message")
        self.boot()
        self.assertFalse((self.module / "disable").exists())
        self.assertFalse((self.module / "skip_mount").exists())
        self.assertFalse(self.log.exists())

    def test_new_platform_service_disables_before_mount(self):
        self.install()
        self.source.write_text(STOCK.replace("* ", "allowlist u:object_r:allowlist_service:s0\n* "))
        self.assert_disabled()

    def test_changed_existing_label_disables(self):
        self.install()
        self.source.write_text(STOCK.replace("activity_service", "different_service"))
        self.assert_disabled()

    def test_missing_stock_disables(self):
        self.install()
        self.source.unlink()
        self.assert_disabled()

    def test_empty_stock_disables(self):
        self.install()
        self.source.write_text("")
        self.assert_disabled()

    def test_missing_hash_disables(self):
        self.install()
        self.hash_file.unlink()
        self.assert_disabled()

    def test_malformed_hash_disables(self):
        self.install()
        for value in ["", "abc", "g" * 64, "a" * 64 + "\nextra", "a" * 65]:
            with self.subTest(value=value):
                self.hash_file.write_text(value)
                self.assert_disabled()

    def test_missing_helper_disables(self):
        self.install()
        (self.module / "service-contexts.sh").unlink()
        self.assert_disabled()

    def test_corrupt_helper_disables(self):
        self.install()
        (self.module / "service-contexts.sh").write_text("if (\n")
        self.assert_disabled()

    def test_existing_disable_markers_are_never_cleared(self):
        self.install()
        for marker in ["disable", "skip_mount"]:
            (self.module / marker).touch()
        self.boot()
        self.assertTrue((self.module / "disable").exists())
        self.assertTrue((self.module / "skip_mount").exists())

    def test_reinstall_while_own_overlay_is_visible_is_idempotent(self):
        self.install()
        expected_hash = self.hash_file.read_text()
        expected_overlay = self.overlay.read_text()
        self.source.write_text(expected_overlay)
        self.install()
        self.assertEqual(self.hash_file.read_text(), expected_hash)
        self.assertEqual(self.overlay.read_text(), expected_overlay)
        self.source.write_text(STOCK)
        self.boot()
        self.assertFalse((self.module / "disable").exists())

    def test_existing_mapping_is_deduplicated_and_moved_before_fallback(self):
        self.source.write_text(STOCK + MAPPING + MAPPING)
        self.install()
        self.assertEqual(self.overlay.read_text().count("anland.host"), 1)
        self.assertLess(self.overlay.read_text().index("anland.host"), self.overlay.read_text().index("* "))

    def test_without_wildcard_mapping_is_appended(self):
        self.source.write_text(STOCK.splitlines(keepends=True)[0])
        self.install()
        self.assertEqual(self.overlay.read_text(), self.source.read_text() + MAPPING)

    def test_missing_source_aborts_install(self):
        self.source.unlink()
        self.assertNotEqual(self.install(check=False).returncode, 0)
        self.assertFalse(self.hash_file.exists())

    def test_missing_fragment_aborts_install(self):
        (self.module / "plat_service_contexts.anland").unlink()
        self.assertNotEqual(self.install(check=False).returncode, 0)

    def test_module_zip_contains_executable_guard_and_hash_helper(self):
        output = self.base / "build"
        output.mkdir()
        (output / "waylandbridge").write_text("inert packaging fixture\n")
        pulse = self.base / "pulse"
        (pulse / "bin").mkdir(parents=True)
        (pulse / "bin/pulseaudio").write_text("inert packaging fixture\n")
        subprocess.run(["make", "-o", "native", "-o", "pulse", "module",
                        "OUT=" + str(output), "PA_ROOT=" + str(pulse)],
                       cwd=ROOT, capture_output=True, text=True, check=True)
        with zipfile.ZipFile(output / "module/anland-awl.zip") as archive:
            for name in ["post-fs-data.sh", "service-contexts.sh", "customize.sh"]:
                self.assertEqual(archive.read(name), (ROOT / "module" / name).read_bytes())
            mode = archive.getinfo("post-fs-data.sh").external_attr >> 16
            self.assertTrue(mode & 0o111)
            self.assertFalse(any("plat_service_contexts.source.sha256" in name for name in archive.namelist()))


if __name__ == "__main__":
    unittest.main()
