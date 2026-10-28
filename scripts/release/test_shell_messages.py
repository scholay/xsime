#!/usr/bin/env python3
"""Keep localized release diagnostics safe under macOS Bash 3.2 nounset."""

import re
import subprocess
import unittest
from pathlib import Path


class LocalizedShellMessageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = (Path(__file__).resolve().parents[1] / "release.sh").read_text()

    def test_braces_delimit_variables_before_non_ascii_punctuation(self):
        self.assertNotRegex(self.script, r"\$[A-Za-z_][A-Za-z_0-9]*[^\x00-\x7f]")

    def test_helper_failures_report_ci_and_android_gates_under_nounset(self):
        start = self.script.index('info "确认 origin/main')
        end = self.script.index('\nif [[ "$KIND" == "stable" ]]; then\n    for formal_environment', start)
        block = self.script[start:end]
        # Exercise the real localized gate block without GitHub calls, a product
        # build, or any repository mutation. Both read-only helper gates fail.
        setup = """KIND=stable; VERSION=1.2.1; fetch_main=aaaaaaaa
info() { :; }
git() { printf 'aaaaaaaa'; }
gate() { printf '%s\\n' "$1"; }
python3() { if [[ "$1" == -c ]]; then printf '24'; else return 1; fi; }
"""
        result = subprocess.run(["/bin/bash", "-uc", setup + block], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("确切 main 提交的原生 CI 未全部通过", result.stdout)
        self.assertIn("Android versionCode 未高于已公开版本", result.stdout)
        self.assertEqual(len(result.stdout.splitlines()), 2)


if __name__ == "__main__":
    unittest.main()
