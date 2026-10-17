"""Exercise real X11 capture, WebM finalization and cancellation in Xvfb."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time
import gi
from gi.repository import Gio, GLib

helper = Path(sys.argv[1]).resolve()
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
              'RequestName', GLib.Variant('(su)', ('org.freedesktop.ScreenSaver', 0)), None, 0, -1, None)
with tempfile.TemporaryDirectory(prefix='xsime-capture-e2e-') as raw:
    root = Path(raw)
    os.chmod(root, 0o700)
    def start(kind, name):
        return subprocess.Popen([str(helper), '--explicit', kind, str(root / name), 'x11'],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, bufsize=1)
    def event(process):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            assert selector.select(15), 'capture event timeout'
        line = process.stdout.readline()
        assert line, process.stderr.read()
        return json.loads(line)
    screenshot = start('image', 'screen.png')
    messages = []
    while not messages or messages[-1]['event'] not in ('finished', 'error'):
        messages.append(event(screenshot))
    assert screenshot.wait(timeout=5) == 0, messages
    assert messages[-1]['width'] == 800 and messages[-1]['height'] == 600, messages
    assert (root / 'screen.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    assert (root / 'screen.png').stat().st_mode & 0o777 == 0o600
    recording = start('video', 'record.webm')
    assert event(recording)['event'] == 'started'
    time.sleep(1)
    recording.stdin.write('stop\n'); recording.stdin.flush()
    value = event(recording)
    assert value['event'] == 'finished' and value['duration'] > 0, value
    assert recording.wait(timeout=5) == 0
    assert (root / 'record.webm').read_bytes().startswith(b'\x1aE\xdf\xa3')
    # Decode actual output, not merely its container signature.
    subprocess.run(['gst-launch-1.0', '-q', 'filesrc', f'location={root / "record.webm"}',
                    '!', 'matroskademux', '!', 'vp8dec', '!', 'fakesink'], check=True, timeout=15)
    recording = start('video', 'cancel.webm')
    assert event(recording)['event'] == 'started'
    recording.stdin.write('cancel\n'); recording.stdin.flush()
    assert event(recording)['event'] == 'cancelled'
    assert recording.wait(timeout=5) != 0 and not (root / 'cancel.webm').exists()
    recording = start('video', 'disconnect.webm')
    assert event(recording)['event'] == 'started'
    recording.stdin.close()
    assert recording.wait(timeout=5) != 0 and not (root / 'disconnect.webm').exists()
    recording = start('video', 'locked.webm')
    assert event(recording)['event'] == 'started'
    time.sleep(.1)
    bus.emit_signal(None, '/org/freedesktop/ScreenSaver', 'org.freedesktop.ScreenSaver',
                    'ActiveChanged', GLib.Variant('(b)', (True,)))
    bus.flush_sync(None)
    assert event(recording)['event'] == 'cancelled'
    assert recording.wait(timeout=5) != 0 and not (root / 'locked.webm').exists()
    existing = root / 'existing.png' ; existing.write_bytes(b'untouched')
    denied = start('image', existing.name)
    assert denied.wait(timeout=5) != 0 and existing.read_bytes() == b'untouched'
    print('X11 PNG, decodable VP8/WebM, explicit stop, cancel, client disconnect, screen-lock signal and existing-file preservation passed')
