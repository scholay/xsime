"""Real screenshot and ScreenCast clients against an isolated D-Bus portal protocol fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import gi
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import Gio, GLib, GdkPixbuf

XML = '''<node>
<interface name="org.freedesktop.portal.Screenshot">
 <property name="version" type="u" access="read"/>
 <method name="Screenshot"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface>
<interface name="org.freedesktop.portal.ScreenCast">
 <property name="version" type="u" access="read"/>
 <property name="AvailableSourceTypes" type="u" access="read"/>
 <property name="AvailableCursorModes" type="u" access="read"/>
 <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface>
</node>'''
CLOSE = '''<node><interface name="org.freedesktop.portal.Request"><method name="Close"/></interface><interface name="org.freedesktop.portal.Session"><method name="Close"/></interface></node>'''
portal_path = '/org/freedesktop/portal/desktop'
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
              'RequestName', GLib.Variant('(su)', ('org.freedesktop.portal.Desktop', 0)), None, 0, -1, None)
context = GLib.MainContext.default()
mode = 'success'
closed = []
requests = []
owners = {}
session_path = ''
registrations = []
close_info = Gio.DBusNodeInfo.new_for_xml(CLOSE)
def close_object(path):
    def close(connection, sender, object_path, interface, method, parameters, invocation):
        closed.append((interface, object_path)); invocation.return_value(None)
    for interface in close_info.interfaces:
        registrations.append(bus.register_object(path, interface, close, None, None))
def response(path, code, results):
    bus.emit_signal(owners[path], path, 'org.freedesktop.portal.Request', 'Response',
                    GLib.Variant('(ua{sv})', (code, results)))
    return False
def method(connection, sender, object_path, interface, name, parameters, invocation):
    global session_path
    args = parameters.unpack(); options = args[-1]
    token = options['handle_token']; owner = sender[1:].replace('.', '_')
    handle = f'{portal_path}/request/{owner}/{token}'
    close_object(handle); owners[handle] = sender; requests.append(name)
    if name == 'Screenshot':
        assert options.get('interactive') is True
        if mode == 'pending':
            invocation.return_value(GLib.Variant('(o)', (handle,))); return
        code = 1 if mode == 'cancel' else 2 if mode == 'deny' else 0
        result = {'uri': GLib.Variant('s', source.as_uri())} if code == 0 else {}
    elif name == 'CreateSession':
        session_path = f'{portal_path}/session/{owner}/{options["session_handle_token"]}'
        close_object(session_path); code = 0; result = {'session_handle': GLib.Variant('s', session_path)}
    elif name == 'SelectSources':
        assert options.get('types') == 3 and options.get('multiple', False) is False
        assert options.get('persist_mode', 0) == 0
        code = 0; result = {}
    elif name == 'Start':
        if mode == 'pending-video':
            invocation.return_value(GLib.Variant('(o)', (handle,))); return
        code = 1; result = {}
    else:
        raise AssertionError(name)
    # Also covers a Response arriving before the method's handle reply.
    response(handle, code, result)
    invocation.return_value(GLib.Variant('(o)', (handle,)))
for interface in Gio.DBusNodeInfo.new_for_xml(XML).interfaces:
    registrations.append(bus.register_object(portal_path, interface, method,
        lambda connection, sender, path, iface, name: GLib.Variant('u', 4 if name == 'version' else 3 if name == 'AvailableSourceTypes' else 1), None))
def spin(predicate, timeout=10):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        while context.pending(): context.iteration(False)
        if predicate(): return
        time.sleep(.01)
    proc.terminate()
    proc.wait(timeout=5)
    out, err = proc.stdout.read(), proc.stderr.read()
    raise AssertionError(('portal test timeout', requests, closed, out, err))
with tempfile.TemporaryDirectory(prefix='xsime-portal-capture-') as raw:
    root = Path(raw); os.chmod(root, 0o700)
    source = root / 'portal.png'
    pixbuf = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, False, 8, 48, 32)
    pixbuf.fill(0x2233ccff); pixbuf.savev(str(source), 'png', [], [])
    def start(kind, target):
        return subprocess.Popen([sys.argv[1], '--explicit', kind, str(root / target), 'portal'],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for mode in ('success', 'cancel', 'deny'):
        proc = start('image', mode + '.png'); spin(lambda: proc.poll() is not None)
        events = [json.loads(line) for line in proc.stdout.read().splitlines()]
        if mode == 'success':
            assert proc.returncode == 0 and events[-1]['event'] == 'finished', events
            assert (root / 'success.png').read_bytes() == source.read_bytes()
        else:
            assert proc.returncode != 0 and not (root / (mode + '.png')).exists(), events
    mode = 'pending'; count = len(requests); proc = start('image', 'pending.png')
    spin(lambda: len(requests) > count)
    proc.stdin.write('cancel\n'); proc.stdin.flush(); spin(lambda: proc.poll() is not None)
    spin(lambda: any(item[0].endswith('Request') for item in closed))
    assert not (root / 'pending.png').exists()
    mode = 'pending-video'; proc = start('video', 'pending.webm')
    spin(lambda: requests[-1] == 'Start')
    proc.stdin.close(); spin(lambda: proc.poll() is not None)
    # The capture helper closes its pending request and its ScreenCast session.
    spin(lambda: any(item == ('org.freedesktop.portal.Session', session_path) for item in closed))
    assert not (root / 'pending.webm').exists()
    # A compositor may revoke permission while its selection dialog is open.
    proc = start('video', 'revoked.webm'); count = len(requests)
    spin(lambda: len(requests) > count and requests[-1] == 'Start')
    owner = owners[next(reversed(owners))]
    bus.emit_signal(owner, session_path, 'org.freedesktop.portal.Session', 'Closed', GLib.Variant('(a{sv})', ({},)))
    spin(lambda: proc.poll() is not None)
    assert not (root / 'revoked.webm').exists()
    print('PNG success/denial/cancel, early Response, ScreenCast without RemoteDesktop, pending Request.Close, Session.Close and session revocation passed')
for registration in registrations: bus.unregister_object(registration)
