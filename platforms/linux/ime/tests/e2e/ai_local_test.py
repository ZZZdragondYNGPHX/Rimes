#!/usr/bin/env python3
"""Local-only HTTP fixture. Never uses a configured provider or real keys."""
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    requests = 0

    def do_GET(self):
        payload = json.dumps({"requests": Handler.requests}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
    def log_message(self, *args):
        pass

    def do_POST(self):
        Handler.requests += 1
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert self.headers.get('Authorization') == 'Bearer local-fixture-key'
        assert body['stream'] is True and body['messages'][1]['role'] == 'user'
        model = body['model']
        status = 401 if model == 'status-401' else 200
        if model in ('slow', 'slow-suggestion'):
            time.sleep(4)
        content = 'fixture-translation' if body['messages'][0]['content'].startswith('Translate') else 'fixture-output'
        if model in ('suggestion', 'slow-suggestion') and 'NEVER translate' in body['messages'][0]['content']:
            source = body['messages'][-1]['content']
            candidate = source.replace('天气气', '天气') if '天气气' in source else source + '散步。'
            content = json.dumps({'text': candidate}, ensure_ascii=False)
        data = {'choices': [{'message': {'content': content}}]}
        if model == 'wrong-shape':
            data = [{'choices': 'invalid', 'secret': 'local-fixture-key'}]
        elif model == 'oversize':
            data['choices'][0]['message']['content'] = 'x' * 32769
        elif model == 'array-content':
            data['choices'][0]['message']['content'] = [{'type': 'text', 'text': 'fixture-array'}]
        elif model == 'status-401':
            data = {'error': {'message': 'local-fixture-key should never be displayed'}}
        if model in ('stream', 'truncated-stream', 'broken-stream'):
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            for piece in ('fixture-', 'output'):
                self.wfile.write(('data: ' + json.dumps({'choices': [{'delta': {'content': piece}}]}) + '\n\n').encode())
                self.wfile.flush()
            if model != 'broken-stream':
                reason = 'length' if model == 'truncated-stream' else 'stop'
                self.wfile.write(('data: ' + json.dumps({'choices': [{'delta': {}, 'finish_reason': reason}]}) + '\n\ndata: [DONE]\n\n').encode())
            return
        payload = b'invalid json'  if model == 'bad-json' else json.dumps(data).encode()
        try:
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    build = Path(sys.argv[1]).resolve()
    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        endpoint = f'http://127.0.0.1:{server.server_port}/v1/chat/completions'
        env = {**os.environ, 'NO_PROXY': '127.0.0.1,localhost'}
        subprocess.run([str(build/'rimes-ai-tests'), endpoint], env=env, check=True)
        if len(sys.argv) > 2:
            subprocess.run([str(build/'rimes-ai-fcitx-e2e'), str(build), endpoint, sys.argv[2]], env=env, check=True)
        server.shutdown()


if __name__ == '__main__':
    main()
