"""Run the vault UI with real key-window ownership in an isolated X11 desktop."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='xsime-vault-desktop-') as raw:
    root = Path(raw)
    env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'config'),
               XDG_CACHE_HOME=str(root / 'cache'), GTK_IM_MODULE='simple', NO_AT_BRIDGE='1')
    wm_env = dict(env)
    wm_env.pop('G_DEBUG', None)
    with (root / 'wm.log').open('w+') as log:
        wm = subprocess.Popen(['xfwm4', '--compositor=off'], env=wm_env, stdout=log, stderr=log)
        try:
            until = time.monotonic() + 8
            while time.monotonic() < until:
                assert wm.poll() is None, 'private window manager exited'
                result = subprocess.run(['xprop', '-root', '_NET_SUPPORTING_WM_CHECK', '_NET_SUPPORTED'],
                                        capture_output=True, text=True, timeout=2)
                if '_NET_ACTIVE_WINDOW' in result.stdout and 'window id # 0x' in result.stdout:
                    break
                time.sleep(.05)
            else:
                raise AssertionError('private window manager startup timed out')
            subprocess.run(sys.argv[1:], env=env, check=True, timeout=120)
        finally:
            wm.terminate()
            try:
                wm.wait(timeout=3)
            except subprocess.TimeoutExpired:
                wm.kill()
                wm.wait()
