"""Exercise the real no-layer-shell display fallback and its teardown."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

build = Path(sys.argv[1]).resolve()
assert os.environ.get('DISPLAY'), 'Run under xvfb-run to provide the fallback X display'
with tempfile.TemporaryDirectory(prefix='xsime-panel-fallback-') as directory:
    root = Path(directory)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root), WAYLAND_DISPLAY='xsime-test',
               GDK_BACKEND='wayland,x11', G_DEBUG='fatal-criticals')
    with (root / 'weston.log').open('w') as log:
        weston = subprocess.Popen(['weston', '--backend=x11', '--renderer=pixman', '--no-config',
                                   '--socket=xsime-test', '--width=1920', '--height=1080', '--idle-time=0'],
                                  env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 8
            while not (root / 'xsime-test').is_socket():
                assert weston.poll() is None and time.monotonic() < deadline, (root / 'weston.log').read_text()
                time.sleep(.05)
            for _ in range(3):
                result = subprocess.run([str(build / 'runtime/rimes-panel-window-tests')], env=env,
                                        text=True, capture_output=True, timeout=15)
                assert result.returncode == 0, result.stdout + result.stderr
                assert 'XWayland' in result.stdout, result.stdout + result.stderr
            print('Weston without layer-shell: fallback placement and repeated display teardown passed with fatal GTK criticals')
        finally:
            weston.terminate()
            try: weston.wait(timeout=3)
            except subprocess.TimeoutExpired: weston.kill(); weston.wait()
