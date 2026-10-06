"""Loopback-only LFS fixture for the Unreal lock integration test; never production."""
import json
import hashlib
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

root = Path(sys.argv[1])
locks = {}
serial = 0
objects = root / 'objects'
objects.mkdir(exist_ok=True)

def lock(path, owner, identity):
    return {'id': identity, 'path': path, 'owner': {'name': owner}, 'locked_at': '2026-10-02T00:00:00Z'}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/vnd.git-lfs+json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except BrokenPipeError:
            pass

    def do_GET(self):
        if self.path.startswith('/objects/'):
            oid = self.path.rsplit('/', 1)[-1]
            if len(oid) != 64 or any(c not in '0123456789abcdef' for c in oid) or not (objects / oid).is_file():
                self.reply(404, {'message': 'object missing'})
                return
            data = (objects / oid).read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif '/locks' in self.path:
            self.reply(200, {'locks': list(locks.values())})
        else:
            self.reply(404, {'message': 'fixture endpoint not found'})

    def do_PUT(self):
        oid = self.path.rsplit('/', 1)[-1]
        size = int(self.headers.get('Content-Length', '0'))
        if not self.path.startswith('/objects/') or len(oid) != 64 or any(c not in '0123456789abcdef' for c in oid) or size > 16 * 1024 * 1024:
            self.reply(400, {'message': 'invalid fixture object'})
            return
        data = self.rfile.read(size)
        if hashlib.sha256(data).hexdigest() != oid:
            self.reply(422, {'message': 'object hash mismatch'})
            return
        (objects / oid).write_bytes(data)
        self.reply(200, {})

    def do_POST(self):
        global serial
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        mode = (root / 'mode').read_text().strip() if (root / 'mode').exists() else ''
        with (root / 'requests').open('a') as log:
            log.write(json.dumps({'path': self.path, 'body': body}) + '\n')
        if self.path.endswith('/objects/batch'):
            if mode == 'uploadfail':
                self.reply(503, {'message': 'fixture upload failed'})
                return
            operation = body.get('operation')
            response = []
            for item in body.get('objects', []):
                oid, size = item['oid'], item['size']
                if len(oid) != 64 or any(c not in '0123456789abcdef' for c in oid):
                    self.reply(400, {'message': 'invalid object ID'})
                    return
                saved = objects / oid
                result = {'oid': oid, 'size': size}
                action = {'href': f'http://127.0.0.1:{self.server.server_port}/objects/{oid}'}
                if operation == 'upload' and not saved.exists():
                    result['actions'] = {'upload': action}
                elif operation == 'download' and saved.exists():
                    result['actions'] = {'download': action}
                elif operation == 'download':
                    result['error'] = {'code': 404, 'message': 'missing object'}
                response.append(result)
            self.reply(200, {'transfer': 'basic', 'objects': response})
        elif self.path.endswith('/locks/verify'):
            if mode == 'timeout':
                time.sleep(3)
                self.reply(200, {'ours': [], 'theirs': []})
            elif mode in ('offline', 'unsupported', 'auth'):
                self.reply({'offline': 503, 'unsupported': 501, 'auth': 403}[mode], {'message': mode})
            elif mode == 'malformed':
                self.reply(200, {'ours': [lock('../escape', 'me', 'bad')], 'theirs': []})
            elif mode == 'duplicate':
                item = lock('asset.uasset', 'me', 'same')
                self.reply(200, {'ours': [item], 'theirs': [item]})
            elif mode in ('pages', 'partial'):
                if not body.get('cursor'):
                    self.reply(200, {'ours': [lock('first.uasset', 'me', 'first')], 'theirs': [], 'next_cursor': 'page2'})
                elif mode == 'partial':
                    self.reply(403, {'message': 'later page refused'})
                else:
                    self.reply(200, {'ours': [], 'theirs': [lock('second.uasset', 'me', 'second')]})
            elif mode == 'foreign':
                # Same display name must never be interpreted as ours.
                self.reply(200, {'ours': [], 'theirs': [lock('asset.uasset', 'UEGit lock fixture', 'foreign')]})
            elif mode == 'otherclone':
                self.reply(200, {'ours': [lock('asset.uasset', 'UEGit lock fixture', 'otherclone')], 'theirs': []})
            else:
                self.reply(200, {'ours': list(locks.values()), 'theirs': []})
        elif self.path.endswith('/locks'):
            path = body['path']
            refused = (root / 'fail-lock-path').read_text().strip() if (root / 'fail-lock-path').exists() else ''
            if path in locks or mode == 'conflict' or path == refused:
                self.reply(409, {'message': 'already locked', 'lock': locks.get(path, lock(path, 'someone else', 'race'))})
            else:
                serial += 1
                locks[path] = lock(path, 'UEGit lock fixture', str(serial))
                if mode == 'postfail':
                    (root / 'mode').write_text('auth')
                self.reply(201, {'lock': locks[path]})
        elif self.path.endswith('/unlock'):
            identity = self.path.split('/')[-2]
            found = next((p for p, item in locks.items() if item['id'] == identity), None)
            refused = (root / 'fail-unlock-path').read_text().strip() if (root / 'fail-unlock-path').exists() else ''
            if found and found == refused:
                self.reply(403, {'message': 'fixture release refused'})
            elif found and not body.get('force'):
                self.reply(200, {'lock': locks.pop(found)})
            else:
                self.reply(403, {'message': 'not owned'})
        else:
            self.reply(404, {'message': 'fixture endpoint not found'})

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
(root / 'port').write_text(str(server.server_port))
server.serve_forever()
