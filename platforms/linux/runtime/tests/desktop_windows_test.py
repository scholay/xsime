"""Real X11 window-manager interaction in a private D-Bus/Xvfb session.

The socket fixture has no text-delivery authority. Fcitx delivery is covered by
the independent host suites; this checks actual window focus and mouse input.
"""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def command(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def wait_for(test, detail):
    until = time.monotonic() + 8
    while time.monotonic() < until:
        result = test()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(detail)


def windows(title):
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", title], capture_output=True, text=True)
    return result.stdout.split()


def geometry(window):
    return dict(line.split("=", 1) for line in command("xdotool", "getwindowgeometry", "--shell", window).splitlines())


def main():
    build = Path(sys.argv[1]).resolve()
    children = []
    scale = int(os.environ.get("GDK_SCALE", "1"))
    with tempfile.TemporaryDirectory(prefix="xsime-window-desktop-") as directory:
        root = Path(directory)
        env = dict(os.environ, RIMES_USER_DIR=str(root / "user"), GTK_IM_MODULE="simple", LANG="C.UTF-8", LC_ALL="C.UTF-8")
        log = (root / "desktop.log").open("w+")

        def start(args):
            process = subprocess.Popen([str(arg) for arg in args], env=env, stdout=log, stderr=log)
            children.append(process)
            return process

        try:
            wm = start(["xfwm4", "--compositor=off"])
            def wm_ready():
                assert wm.poll() is None, "window manager exited during startup"
                result = subprocess.run(["xprop", "-root", "_NET_SUPPORTING_WM_CHECK", "_NET_SUPPORTED"], capture_output=True, text=True)
                return result.returncode == 0 and "_NET_ACTIVE_WINDOW" in result.stdout and "window id # 0x" in result.stdout
            wait_for(wm_ready, "window manager did not publish EWMH support")
            start([build / "runtime/rimes-settings"])
            settings = wait_for(lambda: windows("^XSIME 设置$"), "settings did not open")[0]
            second = start([build / "runtime/rimes-settings"])
            assert second.wait(timeout=8) == 0, "second settings instance did not hand off activation"
            assert windows("^XSIME 设置$") == [settings], "duplicate settings window"
            command("xdotool", "windowactivate", "--sync", settings)
            active = command("xdotool", "getactivewindow")
            server = socket.socket(socket.AF_UNIX)
            server.bind(str(root / "buffer.sock")); server.listen(1); server.settimeout(8)
            start([build / "rimes-buffer", "--socket", root / "buffer.sock", "--dump-geometry", root / "geometry.json"])
            peer, _ = server.accept(); peer.settimeout(3)

            def receive():
                def read(count):
                    value = b""
                    while len(value) < count:
                        part = peer.recv(count - len(value))
                        assert part, "UI disconnected"
                        value += part
                    return value
                return json.loads(read(struct.unpack(">I", read(4))[0]))

            def send(value):
                packet = json.dumps(value).encode()
                peer.sendall(struct.pack(">I", len(packet)) + packet)

            assert receive()["op"] == "hello"
            snapshot = dict(v=1, type="snapshot", visible=True, capturing=True, enabled=True, secure=False,
                            empty=False, width=760, close_after_last=True, theme="night", blocks=[],
                            caret=dict(x=0, y=0, w=0, h=0, valid=False))
            send(snapshot)
            panel = wait_for(lambda: windows("^XSIME Buffer$"), "Buffer did not open")[0]
            wait_for(lambda: (root / "geometry.json").exists(), "missing UI geometry")
            time.sleep(0.2)
            layout = json.loads((root / "geometry.json").read_text())
            switch = layout["controls"]["closeAfterLast"]
            assert (switch["width"], switch["height"]) == (28, 17), switch
            assert command("xdotool", "getactivewindow") == active, "mapping Buffer stole focus"
            frame = geometry(panel)
            x, y = int(frame["X"]), int(frame["Y"])
            assert (int(frame["WIDTH"]), int(frame["HEIGHT"])) == (760 * scale, 73 * scale), frame
            command("xdotool", "mousemove", x + (switch["x"] + 14) * scale, y + (switch["y"] + 8) * scale, "click", "1")
            assert receive()["op"] == "toggle_close_after_last", "first click did not activate compact switch"
            assert command("xdotool", "getactivewindow") == active, "toolbar click stole host focus"
            snapshot["close_after_last"] = False; send(snapshot)

            def toolbar_click():
                time.sleep(0.15)  # let the WM apply both move and resize acknowledgements
                frame = geometry(panel)
                command("xdotool", "mousemove", int(frame["X"]) + 380 * scale, int(frame["Y"]) + 16 * scale, "click", "1")
                assert receive()["op"] == "drag_begin"
                assert receive()["op"] == "drag_end"

            toolbar_click()
            wait_for(lambda: int(geometry(panel)["HEIGHT"]) == 35 * scale, "empty toolbar click did not fold")
            toolbar_click()
            wait_for(lambda: int(geometry(panel)["HEIGHT"]) == 73 * scale, "toolbar click did not expand")
            frame = geometry(panel)
            bottom = int(frame["Y"]) + int(frame["HEIGHT"])
            snapshot.update(active_plugin="fixture", plugins=[dict(id="fixture", name="Translation")],
                            live_expand=True, phase="ready", blocks=[dict(id="source", text="Source", origin="rime")],
                            results=[dict(id="result", text="Result", alternative=0, ready=True)], selected_result="result")
            send(snapshot)
            wait_for(lambda: int(geometry(panel)["HEIGHT"]) == 105 * scale, "dual rail height")
            frame = geometry(panel)
            assert int(frame["Y"]) + int(frame["HEIGHT"]) == bottom, "expansion moved bottom edge"
            snapshot.update(active_plugin="", results=[], blocks=[]); send(snapshot)
            wait_for(lambda: int(geometry(panel)["HEIGHT"]) == 73 * scale, "single rail height")
            frame = geometry(panel)
            command("xdotool", "mousemove", int(frame["X"]) + 380 * scale, int(frame["Y"]) + 16 * scale, "mousedown", "1")
            assert receive()["op"] == "drag_begin"
            command("xdotool", "mousemove_relative", "--", 70 * scale, -60 * scale)
            time.sleep(0.15)
            command("xdotool", "mouseup", "1")
            assert receive()["op"] == "drag_end"
            moved = geometry(panel)
            assert (moved["X"], moved["Y"]) != (frame["X"], frame["Y"]), "WM drag did not move panel"
            assert int(moved["HEIGHT"]) == 73 * scale, "drag incorrectly folded rail"
            wait_for(lambda: (root / "user/windows/buffer.json").exists(), "window position was not saved")
            assert command("xdotool", "getactivewindow") == active, "drag stole host focus"
            peer.close(); server.close()
            print(f"X11 {scale}x: singleton settings, first click, nonactivation, fold, dual-rail resize and drag persistence passed")
        except Exception:
            if len(sys.argv) > 2:
                import gi
                gi.require_version("Gdk", "3.0")
                gi.require_version("Gtk", "3.0")
                from gi.repository import Gdk, Gtk
                Gtk.init([])
                screen = Gdk.get_default_root_window()
                pixels = Gdk.pixbuf_get_from_window(screen, 0, 0, screen.get_width(), screen.get_height())
                if pixels: pixels.savev(sys.argv[2], "png", [], [])
            log.flush(); print((root / "desktop.log").read_text()[-4000:], file=sys.stderr)
            raise
        finally:
            for child in reversed(children):
                if child.poll() is None: child.terminate()
            for child in reversed(children):
                try: child.wait(timeout=3)
                except subprocess.TimeoutExpired: child.kill(); child.wait()
            log.close()


if __name__ == "__main__":
    main()
