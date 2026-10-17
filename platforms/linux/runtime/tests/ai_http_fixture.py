#!/usr/bin/env python3
"""Disposable loopback peer for actual libcurl and Fcitx AI transport tests."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time


def event(text):
    data = {'choices': [{'index': 0, 'delta': {'content': text}, 'finish_reason': None}]}
    return ('data: ' + json.dumps(data, ensure_ascii=False) + '\r\n\r\n').encode()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', '0'))
            assert 0 < length <= 1024 * 1024
            request = json.loads(self.rfile.read(length))
            assert request['model'] == 'fixture-model' and request['stream'] is True
            assert request['messages'][0]['role'] == 'system'
            assert request['messages'][0]['content'].startswith('Return only JSON in this shape:')
            assert request['messages'][1] == {'role': 'user', 'content': '原文🙂'}
            assert self.headers.get('Authorization') == 'Bearer fixture-credential'
            assert self.path.endswith('/chat/completions')
            route = self.path.split('/')[1]
            assert route != 'trap', 'redirect or invalid request reached the trap'
            status = {'auth': 401, 'limited': 429, 'redirect': 302}.get(route, 200)
            self.send_response(status)
            if status == 302:
                self.send_header('Location', '/trap/chat/completions')
            self.send_header('Content-Type', 'text/event-stream; charset=utf-8')
            self.end_headers()
            if status != 200:
                self.wfile.write(b'private server error fixture'); return
            if route in ('timeout', 'cancel'):
                time.sleep(1); return
            if route == 'fragmented':
                raw = ['{"blocks":[{"text":"First 🙂.","title":null},', '{"text":"第二段。","title":null}]}']
                for part in raw:
                    for byte in event(part):
                        self.wfile.write(bytes([byte])); self.wfile.flush()
                    time.sleep(.08)
                self.wfile.write(b'data: [DONE]\n\n'); self.wfile.flush()
                time.sleep(3)  # Client must complete without waiting for EOF.
            elif route == 'incomplete':
                self.wfile.write(event('unfinished'))
            elif route == 'malformed':
                self.wfile.write(b'data: {"invalid":\xed\xa0\x80}\n\n')
            elif route == 'tool':
                self.wfile.write(b'data: {"choices":[{"index":0,"delta":{"tool_calls":[]}}]}\n\n')
            elif route == 'oversized':
                self.wfile.write(event('x' * (256 * 1024 + 1)))
            elif route == 'provider':
                self.wfile.write(b'data: {"error":{"message":"private provider error"}}\n\n')
            else:
                raise AssertionError('unrecognized fixture route')
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.server.failures.append(type(error).__name__)


if __name__ == '__main__':
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    server.failures = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        endpoint = f'http://127.0.0.1:{server.server_port}'
        if sys.argv[1] == '--fcitx':
            build = Path(sys.argv[2]).resolve()
            env = dict(os.environ, RIMES_TEST_AI_API=endpoint)
            result = subprocess.run([str(build / 'rimes-buffer-fcitx-e2e'), str(build), '.', 'test-data'], env=env, timeout=45)
        else:
            result = subprocess.run([sys.argv[1], endpoint], timeout=30)
        if server.failures:
            raise SystemExit('Fixture contract failures: ' + ', '.join(server.failures))
        raise SystemExit(result.returncode)
    finally:
        server.shutdown(); server.server_close()
