"""Explicit integration test with the pinned real engine, isolated from user data.

Downloads ~180-204 MB unless --cache points to verified installer archives.
This is an opt-in CI step, not part of the network-free CTest suite.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--fcitx-build", type=Path)
    parser.add_argument("--shared-data", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xsime-translation-engine-") as directory:
        root = Path(directory)
        env = dict(os.environ, RIMES_USER_DIR=str(root))

        def invoke(request, success=True, offline=False):
            environment = dict(env)
            if offline:
                environment.update(https_proxy="http://127.0.0.1:9", http_proxy="http://127.0.0.1:9", no_proxy="")
            result = subprocess.run([str(args.binary.resolve()), "--request"], input=json.dumps(request),
                                    env=environment, capture_output=True, text=True, timeout=600)
            assert (result.returncode == 0) == success, "Unexpected resource operation status"
            values = [json.loads(line) for line in result.stdout.splitlines()]
            assert values, "Missing resource response"
            return values[-1]

        assert invoke({"op": "status"})["state"] == "missing"
        if args.cache and args.cache.is_dir():
            shutil.copytree(args.cache, root / "translation/cache")
        assert invoke({"op": "install"})["state"] == "ready"
        if args.cache:
            args.cache.mkdir(parents=True, exist_ok=True, mode=0o700)
            shutil.copytree(root / "translation/cache", args.cache, dirs_exist_ok=True)
        assert invoke({"op": "status"}, offline=True)["offline"]
        chinese = "今天天气很好，我们下午去公园散步。"
        english = "Please save the document before closing the window."
        translated = invoke({"op": "translate", "source": "zh-Hans", "target": "en", "text": chinese}, offline=True)["text"]
        assert translated != chinese and "park" in translated.lower(), "Real Chinese to English inference failed"
        translated = invoke({"op": "translate", "source": "en", "target": "zh-Hans", "text": english}, offline=True)["text"]
        assert translated != english and "保存" in translated, "Real English to Chinese inference failed"
        assert "error" in invoke({"op": "translate", "source": "en", "target": "fr", "text": english}, success=False, offline=True)
        translation = root / "translation"
        pointer = json.loads((translation / "current.json").read_text())
        bundle = translation / pointer["bundle"]
        metadata = bundle / "models/translate-zh_en-1_9/metadata.json"
        metadata.write_text("corrupted test model metadata")
        assert invoke({"op": "status"}, offline=True)["state"] == "damaged"
        assert "error" in invoke({"op": "translate", "source": "en", "target": "zh-Hans", "text": english}, success=False, offline=True)
        assert invoke({"op": "install"}, offline=True)["state"] == "ready", "Verified archive repair must work offline"
        assert not list(translation.glob(".staging-*"))
        print("Real pinned engine: bilingual inference, restart, unsupported language, damage refusal and offline repair passed")
        if args.fcitx_build:
            assert args.shared_data and args.shared_data.is_dir(), "Fcitx test requires staged Rime data"
            build = args.fcitx_build.resolve()
            environment = dict(env, RIMES_SHARED_DIR=str(args.shared_data.resolve()), RIMES_TEST_TRANSLATION_RESOURCES=str(translation))
            subprocess.run([str(build / "rimes-buffer-fcitx-e2e"), str(build), ".", "test-data"], env=environment, timeout=210, check=True)


if __name__ == "__main__":
    main()
