"""Require layer-shell tests in an isolated headless Sway compositor."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    assert os.getuid() != 0, "Run compositor acceptance as an unprivileged user"
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="xsime-wayland-panel-") as directory:
        root = Path(directory)
        config = root / "sway.conf"
        config.write_text("output HEADLESS-1 mode 1920x1080 position 0 0\n"
                          "output HEADLESS-2 mode 2560x1440 position 1920 0 scale 2\n"
                          "seat seat0 fallback true\n")
        env = dict(os.environ, XDG_RUNTIME_DIR=str(root), WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   WLR_LIBINPUT_NO_DEVICES="1", WLR_HEADLESS_OUTPUTS="2", GDK_BACKEND="wayland",
                   RIMES_USER_DIR=str(root / "user"), RIMES_PANEL_TEST_SWAY="1")
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "SWAYSOCK", "GDK_SCALE"):
            env.pop(key, None)
        with (root / "sway.log").open("w") as log:
            compositor = subprocess.Popen(["sway", "--unsupported-gpu", "-c", str(config)], env=env, stdout=log, stderr=log)
            try:
                until = time.monotonic() + 8
                while time.monotonic() < until:
                    sockets = [p for p in root.glob("wayland-*") if p.is_socket()]
                    ipc = list(root.glob("sway-ipc.*.sock"))
                    if sockets and ipc:
                        env["WAYLAND_DISPLAY"] = sockets[0].name
                        env["SWAYSOCK"] = str(ipc[0])
                        break
                    assert compositor.poll() is None, (root / "sway.log").read_text()[-3000:]
                    time.sleep(0.05)
                assert "WAYLAND_DISPLAY" in env, "headless compositor did not start"

                def run(*args):
                    result = subprocess.run([str(arg) for arg in args], env=env, text=True, timeout=20, capture_output=True)
                    assert result.returncode == 0, result.stdout + result.stderr
                    return result.stdout

                outputs = json.loads(run("swaymsg", "-t", "get_outputs", "-r"))
                assert len(outputs) == 2 and {o["scale"] for o in outputs} == {1, 2}, outputs
                print(run(build / "runtime/rimes-panel-window-tests").strip())
                for binary, width, height in (("rimes-buffer", 760, 73), ("rimes-capsule", 940, 208)):
                    geometry = root / (binary + ".json")
                    run(build / binary, "--preview", "--dump-geometry", geometry, "--quit-after-dump")
                    value = json.loads(geometry.read_text())
                    assert value["layer"] and (value["width"], value["height"]) == (width, height), value
                run("swaymsg", "output", "HEADLESS-1", "scale", "1.5")
                run("swaymsg", "--", "output HEADLESS-2 position -1280 0")
                print(run(build / "runtime/rimes-panel-window-tests").strip())
                print("Wayland: real layer-shell allocation, mixed/fractional scales, negative origin, monitor selection and disconnect recovery passed")
            finally:
                compositor.terminate()
                try: compositor.wait(timeout=3)
                except subprocess.TimeoutExpired: compositor.kill(); compositor.wait()


if __name__ == "__main__":
    main()
