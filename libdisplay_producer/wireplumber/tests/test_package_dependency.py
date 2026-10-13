"""Regression checks for package version guards and metadata edits."""

import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


HELPER = Path(__file__).resolve().parents[1] / "package-dependency.py"
SPEC = importlib.util.spec_from_file_location("package_dependency", HELPER)
dependency = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dependency)

DEBIAN = """Source: example
Build-Depends: compiler

Package: kwin-common
Architecture: any
Depends: first,
         ${misc:Depends},
Description: Common files
 Continued description.

Package: kwin-data
Architecture: all
Depends: ${misc:Depends}
Description: Other files
"""

RPM = """Name: kwin
Requires: unrelated
%description
Main package.

%package        common
Summary: Common files
Requires: kwin-libs = 1
%description    common
Common package.

%package libs
Summary: Libraries
%description libs
Libraries.
"""


class PackageDependencyTests(unittest.TestCase):
    def test_debian_preserves_other_fields_and_packages(self):
        updated = dependency.add_debian_dependency(DEBIAN, "kwin-common")
        self.assertIn("${misc:Depends},\n " + dependency.DEBIAN_DEPENDENCY, updated)
        self.assertEqual(updated.split("Package: kwin-data")[1],
                         DEBIAN.split("Package: kwin-data")[1])
        self.assertIn("Description: Common files\n Continued description.", updated)

    def test_debian_no_trailing_comma_and_idempotence(self):
        text = DEBIAN.replace("         ${misc:Depends},", "         ${misc:Depends}")
        updated = dependency.add_debian_dependency(text, "kwin-common")
        self.assertIn("${misc:Depends},\n " + dependency.DEBIAN_DEPENDENCY, updated)
        self.assertEqual(dependency.add_debian_dependency(updated, "kwin-common"), updated)

    def test_debian_unsupported_stanzas_fail(self):
        for text in ("Source: kwin\n", DEBIAN + "\nPackage: kwin-common\nDepends: other\n",
                     DEBIAN.replace("Depends: first,\n         ${misc:Depends},", ""),
                     DEBIAN.replace("Depends: first,\n         ${misc:Depends},", "Depends:")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                dependency.add_debian_dependency(text, "kwin-common")

    def test_rpm_dependency_belongs_to_common_and_is_idempotent(self):
        updated = dependency.add_rpm_dependency(RPM, "common")
        self.assertIn("%package        common\nRequires: " + dependency.RPM_DEPENDENCY, updated)
        self.assertEqual(updated.split("%package        common")[0],
                         RPM.split("%package        common")[0])
        self.assertEqual(updated.split("%package libs")[1], RPM.split("%package libs")[1])
        self.assertEqual(dependency.add_rpm_dependency(updated, "common"), updated)

    def test_rpm_unsupported_stanzas_fail(self):
        for text in ("Name: kwin\n", RPM + "\n%package common\nSummary: Duplicate\n",
                     "%package common\nSummary: Missing next section\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                dependency.add_rpm_dependency(text, "common")

    def test_cli_failure_leaves_manifest_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "control"
            manifest.write_text("Source: kwin\n", encoding="utf-8")
            result = subprocess.run([shutil.which("python3"), str(HELPER), "debian",
                                     str(manifest), "kwin-common"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"expected exactly one Debian", result.stderr)
            self.assertEqual(manifest.read_text(), "Source: kwin\n")

    def test_version_guards_exclude_both_regressions(self):
        # Evaluate the metadata's actual relations using an available package
        # manager comparator, rather than a second handwritten version parser.
        dpkg = shutil.which("dpkg")
        vercmp = shutil.which("vercmp")
        if not (dpkg or vercmp):
            self.skipTest("dpkg or vercmp is required for version relation checks")

        def compare(version, operator, bound):
            if dpkg:
                operators = {"<<": "lt", "<": "lt", ">=": "ge"}
                return subprocess.run([dpkg, "--compare-versions", version,
                                       operators[operator], bound]).returncode == 0
            result = int(subprocess.check_output([vercmp, version, bound], text=True))
            return result < 0 if operator in ("<<", "<") else result >= 0

        debian = re.fullmatch(r"wireplumber \((<<) ([^)]+)\) \| wireplumber \((>=) ([^)]+)\)",
                              dependency.DEBIAN_DEPENDENCY).groups()
        rpm = re.fullmatch(r"\(wireplumber (<) (\S+) or wireplumber (>=) (\S+)\)",
                           dependency.RPM_DEPENDENCY).groups()
        for version, allowed in (("0.5.8", True), ("0.5.15", True), ("0.5.16", False),
                                 ("0.5.16-2", False), ("0.5.17", False), ("0.5.17-3", False),
                                 ("0.5.18", True), ("0.5.19", True)):
            for relations in (debian, rpm):
                with self.subTest(version=version, relations=relations):
                    self.assertEqual(compare(version, relations[0], relations[1])
                                     or compare(version, relations[2], relations[3]), allowed)


if __name__ == "__main__":
    unittest.main()
