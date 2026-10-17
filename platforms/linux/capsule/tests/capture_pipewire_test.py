"""Real PipeWire frames and fd transport behind a deterministic ScreenCast portal.
Complements physical portal acceptance; this is not a desktop consent UI test.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from gi.repository import Gio, GLib

XML = '''<node><interface name="org.freedesktop.portal.ScreenCast">
<property name="version" type="u" access="read"/><property name="AvailableSourceTypes" type="u" access="read"/>
<method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="OpenPipeWireRemote"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="h" direction="out"/></method>
</interface></node>'''
CLOSE = '<node><interface name="org.freedesktop.portal.Session"><method name="Close"/></interface></node>'

with tempfile.TemporaryDirectory(prefix='xsime-capture-pipewire-') as raw:
    root = Path(raw);os.chmod(root,0o700)
    env = dict(os.environ,XDG_RUNTIME_DIR=raw, XDG_CONFIG_HOME=str(root/'config'), XDG_CACHE_HOME=str(root/'cache'))
    processes=[];logs=[]
    def launch(name,args,**kwargs):
        log=(root/(name+'.log')).open('w');logs.append(log)
        proc=subprocess.Popen(args,env=env,stderr=log,stdout=log,**kwargs);processes.append(proc);return proc
    bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
    bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','RequestName',
                  GLib.Variant('(su)',('org.freedesktop.portal.Desktop',0)),None,0,-1,None)
    context=GLib.MainContext.default();registrations=[];closed=[];session='';owner='';node={}
    events=[];pending=b'';proc=None
    def spin(predicate,seconds=15):
        global pending
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            while context.pending():context.iteration(False)
            if proc is not None:
                try: data=os.read(proc.stdout.fileno(),65536)
                except BlockingIOError:data=b''
                pending+=data
                while b'\n' in pending:
                    line,pending=pending.split(b'\n',1);events.append(json.loads(line))
            if predicate():return
            time.sleep(.01)
        raise AssertionError(('PipeWire capture timed out',events))
    def method(connection,sender,path,interface,name,parameters,invocation):
        global session,owner
        args=parameters.unpack()
        if name=='Close':closed.append(path);invocation.return_value(None);return
        if name=='OpenPipeWireRemote':
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as remote:
                remote.connect(str(root/'pipewire-0'))
                fds=Gio.UnixFDList.new();index=fds.append(remote.fileno())
                invocation.return_value_with_unix_fd_list(GLib.Variant('(h)',(index,)),fds)
            return
        options=args[-1];owner=sender;prefix='/org/freedesktop/portal/desktop'
        handle=f'{prefix}/request/{sender[1:].replace(".","_")}/{options["handle_token"]}'
        results={}
        if name=='CreateSession':
            session=f'{prefix}/session/{sender[1:].replace(".","_")}/{options["session_handle_token"]}'
            registrations.append(bus.register_object(session,Gio.DBusNodeInfo.new_for_xml(CLOSE).interfaces[0],method,None,None))
            results['session_handle']=GLib.Variant('s',session)
        elif name=='SelectSources':assert options['types']==1 and options['persist_mode']==0
        elif name=='Start':
            props={'pipewire-serial':GLib.Variant('t',node['info']['props']['object.serial'])}
            results['streams']=GLib.Variant('a(ua{sv})',[(node['id'],props)])
        else:raise AssertionError(name)
        invocation.return_value(GLib.Variant('(o)',(handle,)))
        bus.emit_signal(sender,handle,'org.freedesktop.portal.Request','Response',GLib.Variant('(ua{sv})',(0,results)))
    registrations.append(bus.register_object('/org/freedesktop/portal/desktop',Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0],
        method,lambda *args: GLib.Variant('u',6 if args[-1]=='version' else 1),None))
    try:
        launch('pipewire',['pipewire']);spin(lambda:(root/'pipewire-0').is_socket())
        launch('wireplumber',['wireplumber'])
        launch('source',['gst-launch-1.0','-q','videotestsrc','is-live=true','pattern=ball','!',
            'video/x-raw,format=RGB,width=320,height=240,framerate=30/1','!',
            'pipewiresink','mode=provide','stream-properties=props,node.name=xsime-capture-fixture,media.class=Video/Source'])
        def locate():
            global node
            values=json.loads(subprocess.check_output(['pw-dump'],env=env,timeout=3))
            matches=[item for item in values if item.get('info',{}).get('props',{}).get('node.name')=='xsime-capture-fixture']
            if matches:node=matches[0]
            return bool(matches)
        spin(locate)
        for action in ('stop','cancel','revoke'):
            events.clear();pending=b''
            proc=subprocess.Popen([str(Path(sys.argv[1]).resolve()),'--explicit','video',str(root/(action+'.webm')),'portal'],
                env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            processes.append(proc);os.set_blocking(proc.stdout.fileno(),False)
            spin(lambda:events)
            assert events[-1]['event']=='started',events
            started=time.monotonic();spin(lambda:time.monotonic()-started>1)
            if action=='revoke':
                bus.emit_signal(owner,session,'org.freedesktop.portal.Session','Closed',GLib.Variant('(a{sv})',({},)))
            else:proc.stdin.write((action+'\n').encode());proc.stdin.flush()
            spin(lambda:any(item['event'] in ('finished','error','cancelled') for item in events))
            spin(lambda:proc.poll() is not None)
            if action=='stop':
                assert events[-1]['event']=='finished' and events[-1]['width']==320 and events[-1]['height']==240,events
                subprocess.run(['gst-launch-1.0','-q','filesrc',f'location={root / "stop.webm"}','!',
                    'matroskademux','!','vp8dec','!','fakesink'],env=env,check=True,timeout=15)
            else:assert events[-1]['event']=='cancelled' and not (root/(action+'.webm')).exists(),events
            assert session in closed,'ScreenCast session leaked'
            proc=None
        print('Real PipeWire fd/serial targeting, 320x240 video frames, decodable WebM, stop, cancel and portal revocation passed')
    except Exception:
        for log in logs:log.flush()
        for path in root.glob('*.log'):print(path.name,path.read_text()[-4000:],file=sys.stderr)
        if proc and proc.poll() is not None: print(proc.stderr.read(),file=sys.stderr)
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:process.terminate()
            try:process.wait(timeout=3)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        for log in logs:log.close()
        for registration in registrations:bus.unregister_object(registration)
