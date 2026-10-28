"""Test the loopback peer itself; no product binary or input method is executed."""
import http.client
import http.server
import json
import threading
import unittest
from unittest.mock import patch

from ai_http_fixture import Handler, configure


class AIHTTPFixtureTests(unittest.TestCase):
    def start(self, fcitx):
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        configure(server, fcitx=fcitx)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def request(self, server):
        connection = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=2)
        self.addCleanup(connection.close)
        body = json.dumps({'model': 'fixture-model', 'stream': True, 'messages': [
            {'role': 'system', 'content': 'Return only JSON in this shape: fixture'},
            {'role': 'user', 'content': '原文🙂'}]}, ensure_ascii=False).encode()
        connection.request('POST', '/fragmented/chat/completions', body,
                           {'Authorization': 'Bearer fixture-credential'})
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.addCleanup(response.close)
        return connection, response

    def complete(self, server):
        connection, response = self.request(server)
        lines = []
        while True:
            line = response.readline()
            self.assertTrue(line, 'normal fixture ended before DONE')
            lines.append(line)
            if line.strip() == b'data: [DONE]': break
        response.close(); connection.close()
        self.assertIn('First 🙂.'.encode(), b''.join(lines))
        self.assertIn('第二段。'.encode(), b''.join(lines))

    def test_standalone_transport_keeps_both_completed_responses(self):
        server = self.start(False)
        self.complete(server); self.complete(server)
        self.assertFalse(server.cancel_preview_closed.is_set())
        self.assertFalse(server.failures)

    def test_only_second_fcitx_request_waits_for_client_cancellation(self):
        server = self.start(True)
        self.complete(server)
        connection, response = self.request(server)
        first = response.readline()
        self.assertIn('First 🙂.'.encode(), first)
        # Multiple heartbeats must not finalize the second request.
        for _ in range(8): self.assertNotIn(b'[DONE]', response.readline())
        response.close(); connection.close()
        self.assertTrue(server.cancel_preview_closed.wait(2))
        self.assertEqual(server.fragmented_requests, 2)
        self.assertFalse(server.failures)

    def test_missing_cancellation_is_bounded_and_fails_the_fixture(self):
        server = self.start(True)
        self.complete(server)
        with patch('ai_http_fixture.CANCEL_PREVIEW_TIMEOUT', .15):
            _, response = self.request(server)
            data = response.read()
        self.assertNotIn(b'[DONE]', data)
        self.assertFalse(server.cancel_preview_closed.is_set())
        self.assertEqual(server.failures, ['AssertionError'])


if __name__ == '__main__': unittest.main()
