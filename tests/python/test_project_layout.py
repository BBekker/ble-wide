# SPDX-License-Identifier: MIT
"""Package entry points and compatibility with existing router launchers."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ProjectLayoutTest(unittest.TestCase):
    def test_package_and_legacy_cli_have_the_same_options(self):
        module = subprocess.check_output([sys.executable, '-m', 'airspace', '--help'], cwd=ROOT, text=True)
        with tempfile.TemporaryDirectory() as directory:
            legacy = subprocess.check_output([sys.executable, str(ROOT/'server.py'), '--help'], cwd=directory, text=True)
        self.assertEqual(module, legacy)
        self.assertIn('--transport', module)

    def test_router_import_and_resources_resolve_to_package(self):
        from airspace.live_receiver import detect_adapter
        from airspace.paths import ROOT as package_root, WEB
        import live_receiver
        self.assertIs(live_receiver.detect_adapter, detect_adapter)
        self.assertEqual(package_root, ROOT)
        self.assertTrue((WEB/'index.html').is_file())
        self.assertTrue((ROOT/'receiver/Makefile').is_file())


if __name__ == '__main__':
    unittest.main()
