"""Real Sway, PipeWire and xdg-desktop-portal-wlr capture in a private desktop.
The chooser is disabled only inside this test's private configuration directory.
"""
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time


def main():
    assert os.getuid() != 0, 'Run Sway as an unprivileged user'
    helper = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='xsime-capture-wayland-') as raw:
        root = Path(raw)
        config = root / 'config'; config.mkdir()
        (config / 'xdg-desktop-portal').mkdir()
        (config / 'xdg-desktop-portal' / 'portals.conf').write_text('[preferred]\ndefault=wlr;gtk\norg.freedesktop.impl.portal.Access=gtk\n')
        (config / 'xdg-desktop-portal-wlr').mkdir()
        (config / 'xdg-desktop-portal-wlr' / 'config').write_text(
            '[screencast]\noutput_name=HEADLESS-1\nchooser_type=none\nmax_fps=30\n')
        (root / 'sway.conf').write_text('output HEADLESS-1 mode 800x600\noutput HEADLESS-1 bg #2255aa solid_color\nseat seat0 fallback true\n')
        env = dict(os.environ, XDG_RUNTIME_DIR=str(root), XDG_CONFIG_HOME=str(config),
                   XDG_CACHE_HOME=str(root / 'cache'), XDG_CURRENT_DESKTOP='sway', XDG_SESSION_TYPE='wayland',
                   WLR_BACKENDS='headless', WLR_RENDERER='pixman', WLR_LIBINPUT_NO_DEVICES='1', WLR_HEADLESS_OUTPUTS='1')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'SWAYSOCK'): env.pop(key, None)
        processes = []
        logs = []
        def launch(name, args):
            log = (root / (name + '.log')).open('w'); logs.append(log)
            process = subprocess.Popen(args, env=env, stdout=log, stderr=log)
            processes.append(process); return process
        def wait(predicate, seconds=10):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                if predicate(): return
                time.sleep(.05)
            raise AssertionError('isolated Wayland startup timed out')
        try:
            # Own the document portal too: its FUSE mount must be unmounted
            # before TemporaryDirectory cleanup on CI hosts with /dev/fuse.
            launch('documents', ['/usr/libexec/xdg-document-portal', '--replace'])
            launch('sway', ['sway', '--unsupported-gpu', '-c', str(root / 'sway.conf')])
            wait(lambda: any(path.is_socket() for path in root.glob('wayland-*')))
            env['WAYLAND_DISPLAY'] = next(path.name for path in root.glob('wayland-*') if path.is_socket())
            subprocess.run(['dbus-update-activation-environment', '--all'], env=env, check=True)
            launch('pipewire', ['pipewire'])
            wait(lambda: (root / 'pipewire-0').is_socket())
            launch('wireplumber', ['wireplumber'])
            launch('wlr-portal', ['/usr/libexec/xdg-desktop-portal-wlr', '-l', 'DEBUG'])
            launch('portal', ['/usr/libexec/xdg-desktop-portal', '--replace'])
            def available():
                result = subprocess.run(['gdbus', 'introspect', '--session', '--dest',
                    'org.freedesktop.portal.Desktop', '--object-path', '/org/freedesktop/portal/desktop'],
                    env=env, capture_output=True, text=True, timeout=5)
                return 'interface org.freedesktop.portal.ScreenCast' in result.stdout
            wait(available)
            def start(kind, name):
                process = subprocess.Popen([str(helper), '--explicit', kind, str(root / name), 'portal'],
                    env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
                processes.append(process); return process
            def event(process):
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    assert selector.select(20), 'portal capture response timeout'
                line = process.stdout.readline()
                assert line, process.stderr.read()
                return json.loads(line)
            shot = start('image', 'screen.png')
            # Drive the real slurp region selector only on this private Sway
            # seat. The production helper still requests interactive consent.
            time.sleep(.8)
            subprocess.run([str(helper.parent / 'rimes-capture-pointer-fixture')],
                env=dict(env, RIMES_PRIVATE_WAYLAND_CAPTURE_TEST='1'), check=True, timeout=5)
            value = event(shot)
            assert value['event'] == 'finished' and value['width'] == 401 and value['height'] == 301, value
            assert shot.wait(timeout=5) == 0
            assert (root / 'screen.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
            for action in ('stop', 'cancel'):
                recording = start('video', action + '.webm')
                value = event(recording)
                if value['event'] == 'error' and '--allow-headless-no-dmabuf' in sys.argv:
                    # xdpw 0.7 requires a DMA-BUF format even when it could use
                    # SHM. Sway's software-only renderer has no DRM device.
                    # Preserve this acceptance gap; never call it a recording pass.
                    backend_log=(root / 'wlr-portal.log').read_text()
                    assert 'unable to receive a valid format from wlr_screencopy' in backend_log, value
                    assert recording.wait(timeout=5) != 0 and not (root / (action + '.webm')).exists()
                    print('Sway interactive PNG screenshot passed. Physical screencast remains unverified: xdpw 0.7 rejected the headless renderer without DMA-BUF. Rejection cleaned the session and unfinished file.')
                    return
                assert value['event'] == 'started', value
                time.sleep(1)
                recording.stdin.write(action + '\n'); recording.stdin.flush()
                value = event(recording)
                assert value['event'] == ('finished' if action == 'stop' else 'cancelled'), value
                recording.wait(timeout=5)
                if action == 'stop':
                    assert recording.returncode == 0 and value['width'] == 800 and value['height'] == 600, value
                    subprocess.run(['gst-launch-1.0', '-q', 'filesrc', f'location={root / "stop.webm"}',
                        '!', 'matroskademux', '!', 'vp8dec', '!', 'fakesink'], env=env, check=True, timeout=15)
                else:
                    assert recording.returncode != 0 and not (root / 'cancel.webm').exists()
            print('Real Sway portal PNG, PipeWire VP8/WebM, decode, stop, cancel and private output passed')
        except Exception:
            for log in logs: log.flush()
            for path in root.glob('*.log'): print(path.name, path.read_text()[-4500:], file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None: process.terminate()
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            document_mount = root / 'doc'
            if document_mount.is_mount():
                subprocess.run(['fusermount3', '-u', str(document_mount)], check=True, timeout=5)
            for log in logs: log.close()

if __name__ == '__main__':
    main()
