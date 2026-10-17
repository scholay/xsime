"""Exercise actual multi-MIME clipboard I/O in an isolated Sway compositor."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    assert os.getuid() != 0, "Run Sway as an unprivileged user"
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xsime-clipboard-wayland-") as directory:
        root = Path(directory)
        config = root / "sway.conf"
        config.write_text("output HEADLESS-1 mode 1280x720\nseat seat0 fallback true\n")
        env = dict(os.environ, XDG_RUNTIME_DIR=str(root), WLR_BACKENDS="headless",
                   WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1", WLR_HEADLESS_OUTPUTS="1")
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "SWAYSOCK"):
            env.pop(key, None)
        with (root / "sway.log").open("w") as log:
            sway = subprocess.Popen(["sway", "--unsupported-gpu", "-c", str(config)],
                                    env=env, stdout=log, stderr=log)
            try:
                until = time.monotonic() + 8
                while time.monotonic() < until:
                    sockets = [path for path in root.glob("wayland-*") if path.is_socket()]
                    if sockets:
                        env["WAYLAND_DISPLAY"] = sockets[0].name
                        break
                    assert sway.poll() is None, (root / "sway.log").read_text()[-3000:]
                    time.sleep(0.05)
                assert "WAYLAND_DISPLAY" in env, "Sway startup timed out"
                result = subprocess.run([str(build / "rimes-clipboard-desktop-tests")], env=env,
                                        capture_output=True, text=True, timeout=35)
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout.strip())
            finally:
                sway.terminate()
                try:
                    sway.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    sway.kill()
                    sway.wait()


if __name__ == "__main__":
    main()
