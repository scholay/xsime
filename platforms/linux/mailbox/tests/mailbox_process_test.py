"""Exercise the primary application, secondary launches and notification action on a private desktop."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary, fixture_file = sys.argv[1:]
fixture = json.loads(Path(fixture_file).read_text())

def wait(predicate, reason, seconds=8):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError(reason)

with tempfile.TemporaryDirectory(prefix='xsime-mailbox-process-') as temporary:
    root = Path(temporary)
    mailbox = root / 'mailbox'
    mailbox.mkdir(mode=0o700)
    data = mailbox / 'mailbox.json'
    data.write_text(json.dumps(fixture['document']))
    data.chmod(0o600)
    env = dict(os.environ, RIMES_USER_DIR=str(root), GTK_IM_MODULE='simple', NO_AT_BRIDGE='1')
    identity = 'org.scholay.rimes.Mailbox.p' + hashlib.sha256(str(root.resolve()).encode()).hexdigest()[:24]
    def command(*args, **kwargs):
        return subprocess.run(args, env=env, check=True, capture_output=True, text=True, timeout=8, **kwargs)
    def windows():
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '^Mailbox$'], env=env,
                                capture_output=True, text=True, timeout=2)
        return result.stdout.split()
    def read(thread_id):
        document = json.loads(data.read_text())
        return next(t for t in document['threads'] if t['id'].lower() == thread_id)['unread'] is False
    with (root / 'process.log').open('w+') as log:
        process = subprocess.Popen([binary, '--thread', fixture['selectedInbox']], env=env, stdout=log, stderr=log)
        try:
            wait(lambda: len(windows()) == 1 and read(fixture['selectedInbox']), 'initial window did not become visible/read')
            window = windows()[0]
            assert int(command('xdotool', 'getwindowpid', window).stdout.strip()) == process.pid
            command(binary, '--module', 'chat', '--thread', fixture['selectedChat'])
            wait(lambda: read(fixture['selectedChat']), 'secondary thread request did not reach primary window')
            assert windows() == [window], 'secondary request created a second window'
            # Same GAction target used by a desktop notification, without posting a toast.
            command('gapplication', 'action', identity, 'open-thread', "'" + fixture['selectedInbox'] + "'")
            assert windows() == [window], 'notification action created a second window'
            command('xdotool', 'windowactivate', '--sync', window)
            command('xdotool', 'windowsize', window, '740', '560')
            def geometry_saved():
                path = mailbox / 'window.json'
                return path.exists() and (json.loads(path.read_text())['width'], json.loads(path.read_text())['height']) == (740,560)
            wait(geometry_saved, 'window dimensions were not persisted')
            command('xdotool', 'key', 'ctrl+w')
            wait(lambda: not windows(), 'close did not hide window')
            assert process.poll() is None, 'closing window terminated its host'
            alias_env = dict(env, RIMES_USER_DIR=str(root / '..' / root.name))
            subprocess.run([binary, '--show'], env=alias_env, check=True, capture_output=True, timeout=8)
            wait(lambda: windows() == [window], 'same-profile alias created another host')
            conflict = subprocess.run(['dbus-run-session', '--', binary, '--show'], env=env,
                                      capture_output=True, text=True, timeout=8)
            assert conflict.returncode != 0, 'a second bus acquired the same profile primary lock'
            assert process.poll() is None, 'primary exited after the cross-bus conflict'
            # The rejected process can still have X unmap/destroy events queued
            # when waitpid returns. Observe the primary after the display settles.
            wait(lambda: process.poll() is None and windows() == [window],
                 'primary window did not survive cross-bus conflict: ' + repr(windows()), seconds=2)
            before = data.read_bytes()
            command(binary, '--quit')
            process.wait(timeout=5)
            assert process.returncode == 0 and data.read_bytes() == before, 'quit changed persistent messages'
            print('Mailbox process: one window/profile, deep links, notification action, hide/reopen, geometry and cross-bus ownership passed')
        except BaseException:
            log.flush(); log.seek(0)
            print(log.read()[-8000:], file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
