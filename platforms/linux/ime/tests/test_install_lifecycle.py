"""Exercise the real shell entry points and CMake install in disposable prefixes."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
SCRIPTS = REPO / "platforms/linux/ime/scripts"
RECEIPT = "share/rimes/ime-install-manifest.json"


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for installer lifecycle tests")
class InstallLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rimes-install-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.prefix = self.root / "local prefix"
        self.target = self.prefix
        self.build = self.root / "build"
        self.source = self.root / "source"
        self.stage = self.root / "data"
        self.source.mkdir()
        self.stage.mkdir()
        for name in ("default.yaml", "custom_phrase.txt"):
            shutil.copyfile(REPO / "rime-data" / name, self.stage / name)
        (self.source / "program").write_text("#!/bin/sh\nexit 0\n")
        (self.source / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.16)
project(InstallFixture NONE)
install(PROGRAMS program DESTINATION bin RENAME rimes-schemes)
install(PROGRAMS program DESTINATION bin RENAME rimes-settings)
install(PROGRAMS program DESTINATION bin RENAME rimes-translation)
install(PROGRAMS program DESTINATION bin RENAME rimes-launch)
install(PROGRAMS program DESTINATION bin RENAME rimes-mailbox)
install(PROGRAMS program DESTINATION bin RENAME rimes-gateway)
install(FILES program DESTINATION libexec/rimes/translation RENAME runtime.py)
install(FILES program DESTINATION libexec/rimes/translation RENAME translation_core.py)
install(FILES program DESTINATION libexec/rimes/translation RENAME resources.lock.json)
install(FILES program DESTINATION libexec/rimes/translation RENAME NOTICE.md)
install(PROGRAMS program DESTINATION libexec/rimes RENAME rimes-buffer)
install(PROGRAMS program DESTINATION libexec/rimes RENAME rimes-deploy)
install(PROGRAMS program DESTINATION libexec/rimes RENAME rimes-capsule-preview)
install(PROGRAMS program DESTINATION libexec/rimes RENAME rimes-capsule-capture)
install(PROGRAMS program DESTINATION lib/x86_64-linux-gnu/fcitx5 RENAME rimes.so)
install(FILES program DESTINATION share/fcitx5/addon RENAME rimes.conf)
install(FILES program DESTINATION share/fcitx5/inputmethod RENAME rimes.conf)
install(FILES program DESTINATION share/icons/hicolor/48x48/apps RENAME rimes.png)
install(FILES program DESTINATION share/licenses/rimes RENAME LICENSE.txt)
install(FILES program DESTINATION share/applications RENAME org.scholay.rimes.schemes.desktop)
install(FILES program DESTINATION share/applications RENAME org.scholay.rimes.settings.desktop)
install(FILES program DESTINATION share/applications RENAME org.scholay.rimes.buffer.desktop)
install(FILES program DESTINATION share/applications RENAME org.scholay.rimes.capsule.desktop)
install(FILES program DESTINATION share/applications RENAME org.scholay.rimes.mailbox.desktop)
''')
        self.env = {k: v for k, v in os.environ.items()
                    if k not in {"DESTDIR", "RIMES_IME_PREFIX", "RIMES_IME_BUILD_DIR"}}
        self.env["RIMES_IME_STAGE_DIR"] = str(self.stage)

    def run_command(self, *args, ok=True):
        result = subprocess.run([str(a) for a in args], env=self.env,
                                text=True, capture_output=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def install(self):
        self.run_command("cmake", "-S", self.source, "-B", self.build,
                         f"-DCMAKE_INSTALL_PREFIX={self.prefix}")
        self.run_command(SCRIPTS / "install.sh", "--skip-build", "--build-dir",
                         self.build, "--prefix", self.prefix)

    def uninstall(self, *args, ok=True):
        return self.run_command(SCRIPTS / "uninstall.sh", "--prefix", self.prefix, *args, ok=ok)

    def receipt(self):
        return json.loads((self.target / RECEIPT).read_text())

    def sentinel(self, relative):
        path = self.target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"personal data - preserve exactly\n")
        return path

    def test_roundtrip_without_build_preserves_all_personal_data(self):
        personal = [self.sentinel(p) for p in (
            "share/rimes/rime_ice.userdb/000001.log", "share/rimes/user.yaml",
            "share/rimes/schemes/learning/wanxiang.userdb/CURRENT",
            "share/rimes/schemes/packages/custom/source.txt", "share/rimes/capsule/notes.md",
            "share/rimes/capsule/clipboard/history-v1.sqlite3",
            "share/rimes/capsule/master-key", "share/rimes/capsule/reveal-credential-v1",
            "share/rimes/capsule/passwords/fixture.md",
            "share/rimes/gateway/token", "share/rimes/gateway/status.json",
            "share/rimes/mailbox/mailbox.json", "share/rimes/mailbox/images/fixture.png", "share/rimes/mailbox/window.json",
            "share/rimes/capsule/captures/example/record.json", "share/rimes/capsule/captures/example/original.webm",
            "share/rimes/capsule/assets/reference.asset", "share/rimes/capsule/reference-style-v1",
            "share/rimes/data/personal.yaml", "share/fcitx5/addon/another.conf",
            "bin/another-program", ".engine/install/lib/librime.so",
            "share/rimes/translation/current.json", "share/rimes/translation/cache/model.argosmodel",
        )]
        before = {p: p.read_bytes() for p in personal}
        self.install()
        installed = list(self.receipt()["files"])
        self.assertIn("share/rimes/data/default.yaml", installed)
        self.assertIn("lib/x86_64-linux-gnu/fcitx5/rimes.so", installed)
        self.assertIn("libexec/rimes/rimes-deploy", installed)
        self.assertIn("libexec/rimes/rimes-capsule-preview", installed)
        self.assertIn("libexec/rimes/rimes-capsule-capture", installed)
        self.assertIn("libexec/rimes/translation/runtime.py", installed)
        self.assertTrue(os.access(self.target / "bin/rimes-translation", os.X_OK))
        self.assertTrue(os.access(self.target / "bin/rimes-launch", os.X_OK))
        self.assertTrue(os.access(self.target / "bin/rimes-mailbox", os.X_OK))
        self.assertTrue(os.access(self.target / "bin/rimes-gateway", os.X_OK))
        self.assertIn("share/applications/org.scholay.rimes.mailbox.desktop", installed)
        self.assertIn("share/applications/org.scholay.rimes.buffer.desktop", installed)
        self.assertIn("share/applications/org.scholay.rimes.capsule.desktop", installed)
        self.assertTrue(os.access(self.target / "libexec/rimes/rimes-deploy", os.X_OK))
        self.assertTrue(os.access(self.target / "bin/rimes-schemes", os.X_OK))
        shutil.rmtree(self.build)
        self.uninstall()
        self.assertTrue(all(not (self.target / p).exists() for p in installed))
        self.assertFalse((self.target / RECEIPT).exists())
        self.assertEqual(before, {p: p.read_bytes() for p in personal})
        self.uninstall()  # Already absent is a safe no-op.

    def test_dry_run_is_read_only_and_missing_files_are_tolerated(self):
        self.install()
        before = {p: (self.target / p).read_bytes() for p in self.receipt()["files"]}
        receipt = (self.target / RECEIPT).read_bytes()
        result = self.uninstall("--dry-run")
        self.assertIn("Would remove:", result.stdout)
        self.assertEqual(receipt, (self.target / RECEIPT).read_bytes())
        self.assertEqual(before, {p: (self.target / p).read_bytes() for p in before})
        (self.target / "bin/rimes-schemes").unlink()
        self.uninstall()

    def test_system_prefix_with_destdir_never_touches_host(self):
        self.prefix = Path("/usr")
        self.env["DESTDIR"] = str(self.root / "package root")
        self.target = Path(self.env["DESTDIR"]) / "usr"
        self.install()
        self.assertEqual(self.receipt()["prefix"], "/usr")
        self.uninstall()
        self.assertFalse((self.target / "bin/rimes-schemes").exists())

    def test_upgrade_keeps_ownership_of_retired_files(self):
        self.install()
        (self.stage / "custom_phrase.txt").unlink()
        (self.source / "program").write_text("#!/bin/sh\nexit 1\n")
        self.install()
        self.assertIn("share/rimes/data/custom_phrase.txt", self.receipt()["files"])
        self.uninstall()
        self.assertFalse((self.target / "share/rimes/data/custom_phrase.txt").exists())

    def test_modified_file_stops_before_any_deletion(self):
        self.install()
        modified = self.target / "share/rimes/data/default.yaml"
        original = modified.read_bytes()
        modified.write_text("personal customizations\n")
        self.assertIn("nothing removed", self.uninstall(ok=False).stderr)
        self.assertTrue((self.target / "bin/rimes-schemes").exists())
        self.assertTrue((self.target / RECEIPT).exists())
        modified.write_bytes(original)
        self.uninstall()

    def test_manifest_cannot_target_user_data_or_escape_prefix(self):
        self.install()
        receipt = self.receipt()
        for bad in ("share/rimes/schemes/learning/userdb/CURRENT", "../outside",
                    "/etc/passwd", "share/rimes/data/test.userdb/CURRENT"):
            with self.subTest(path=bad):
                value = dict(receipt, files={**receipt["files"], bad: "0" * 64})
                (self.target / RECEIPT).write_text(json.dumps(value))
                self.uninstall(ok=False)
                self.assertTrue((self.target / "bin/rimes-schemes").exists())
        (self.target / RECEIPT).write_text(json.dumps(receipt))
        self.uninstall()

    def test_replaced_parent_symlink_preserves_external_files(self):
        self.install()
        original = self.target / "share/rimes/data"
        external = self.root / "external data"
        original.rename(external)
        original.symlink_to(external, target_is_directory=True)
        before = (external / "default.yaml").read_bytes()
        self.assertIn("symlink", self.uninstall(ok=False).stderr)
        self.assertEqual(before, (external / "default.yaml").read_bytes())
        self.assertTrue((self.target / "bin/rimes-schemes").exists())

    def test_replaced_file_symlink_stops_uninstall_and_install(self):
        self.install()
        program = self.target / "bin/rimes-schemes"
        external = self.root / "external program"
        external.write_text("unrelated")
        program.unlink()
        program.symlink_to(external)
        self.uninstall(ok=False)
        self.run_command(SCRIPTS / "install.sh", "--skip-build", "--build-dir", self.build,
                         "--prefix", self.prefix, ok=False)
        self.assertEqual(external.read_text(), "unrelated")

    def test_corrupt_or_wrong_prefix_receipt_stops_without_deletion(self):
        self.install()
        receipt = self.receipt()
        for value in ("{", "[]", json.dumps(dict(receipt, prefix="/wrong"))):
            (self.target / RECEIPT).write_text(value)
            self.uninstall(ok=False)
            self.assertTrue((self.target / "bin/rimes-schemes").exists())

    def test_legacy_install_uses_original_manifest_and_known_data_only(self):
        self.install()
        (self.target / RECEIPT).unlink()
        user = self.sentinel("share/rimes/rime_ice.userdb/CURRENT")
        extra = self.sentinel("share/rimes/data/extra.yaml")
        self.uninstall("--build-dir", self.build, "--dry-run")
        self.assertTrue((self.target / "bin/rimes-schemes").exists())
        self.uninstall("--build-dir", self.build)
        self.assertFalse((self.target / "bin/rimes-schemes").exists())
        self.assertFalse((self.target / "share/rimes/data/default.yaml").exists())
        self.assertTrue(user.exists() and extra.exists())

    def test_legacy_manifest_with_wrong_prefix_is_rejected(self):
        self.install()
        (self.target / RECEIPT).unlink()
        with (self.build / "install_manifest.txt").open("a") as output:
            output.write("\n/elsewhere/bin/rimes-schemes\n")
        self.uninstall("--build-dir", self.build, ok=False)
        self.assertTrue((self.target / "bin/rimes-schemes").exists())

    def test_skip_build_cannot_change_the_compiled_prefix(self):
        self.install()
        other = self.root / 'different prefix'
        result = self.run_command(SCRIPTS / 'install.sh', '--skip-build', '--build-dir', self.build,
                                 '--prefix', other, ok=False)
        self.assertIn('differs from the build', result.stderr)
        self.assertFalse((other / RECEIPT).exists())

    def test_cli_rejects_missing_values_unknown_options_and_root(self):
        for option in ("--prefix", "--build-dir", "--purge-user-data"):
            self.run_command(SCRIPTS / "uninstall.sh", option, ok=False)
        self.run_command(SCRIPTS / "uninstall.sh", "--prefix", "/", ok=False)
        self.run_command(SCRIPTS / "uninstall.sh", "--prefix", "relative", ok=False)


if __name__ == "__main__":
    unittest.main()
