"""Real, credential-free native provider smoke. Uses only an isolated temporary profile."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--bin", type=Path, required=True)
parser.add_argument("--cache", type=Path)
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8")
binary = args.bin.resolve()
with tempfile.TemporaryDirectory(prefix="rimes-local-translation-test-") as directory:
    profile = Path(directory).resolve()
    root = profile / "RIMES/translation/ct2-4.6-argos-1.9-v1"
    command = [str(Path(os.environ["SYSTEMROOT"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"),
               "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
               str(binary / "translation/Install-LocalTranslation.ps1"), "-Root", str(root)]
    if args.cache:
        command += ["-Cache", str(args.cache.resolve())]
    setup_environment = dict(os.environ)
    setup_environment.pop("PSModulePath", None)
    try:
        setup = subprocess.run(command, env=setup_environment, timeout=600, capture_output=True)
    except subprocess.TimeoutExpired as error:
        # The installer logs only logical resource names, byte counts and hashes;
        # retain those bounded diagnostics even when its outer deadline fires.
        for value in (error.stdout, error.stderr):
            if value:
                print(value[-128 * 1024:].decode("utf-8", errors="replace"), flush=True)
        print("Translation setup exceeded its 600-second deadline; no unverified resource was accepted.", flush=True)
        raise
    print(setup.stdout.decode("utf-8", errors="replace"), flush=True)
    if setup.returncode:
        print(setup.stderr.decode("utf-8", errors="replace"))
        setup.check_returncode()
    environment = dict(os.environ, LOCALAPPDATA=str(profile))
    cases = [("English", "今天天气很好，我们下午去公园散步。"),
             ("中文", "Please save the document before closing the window.")]
    evidence = []
    for language, source in cases:
        output = subprocess.run([str(binary / "RimesLocalTranslationProbe.exe"), language, source],
                                env=environment, check=True, timeout=120, capture_output=True)
        text = output.stdout.decode("utf-8")
        assert text and text != source and "▁" not in text and "\ufffd" not in text, text
        if language == "English":
            assert "park" in text.lower() and "afternoon" in text.lower(), text
        else:
            assert "保存" in text and "窗口" in text, text
        evidence.append(dict(target=language, source=source, result=text))
    bad = subprocess.run([str(binary / "RimesLocalTranslationProbe.exe"), "Unsupported", "Hello."],
                         env=environment, timeout=120, capture_output=True)
    assert bad.returncode != 0 and not bad.stdout, "Unsupported language must fail without a source fallback"
    print(json.dumps(dict(passed=True, connector="local", cases=evidence), ensure_ascii=False, indent=2))
