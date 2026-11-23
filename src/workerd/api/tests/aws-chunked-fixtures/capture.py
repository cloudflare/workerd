# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Records S3 PutObject/UploadPart request bodies exactly as clients send them.

Usage: capture.py OUT_DIR HTTP_PORT HTTPS_PORT CERT KEY
Each request to /<bucket>/<key> is saved as OUT_DIR/<key>.json (headers) and OUT_DIR/<key>.bin
(the body after HTTP-level framing is removed).
"""

import http.server
import json
import ssl
import sys
import threading
from pathlib import Path


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def read_body(self):
        if self.headers.get("transfer-encoding", "").lower() == "chunked":
            parts = []
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip(), 16)
                if size == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    return b"".join(parts)
                parts.append(self.rfile.read(size))
                self.rfile.readline()
        return self.rfile.read(int(self.headers.get("content-length", "0")))

    def reply(self, body=b"", status=200):
        self.send_response(status)
        self.send_header("ETag", '"0123456789abcdef0123456789abcdef"')
        self.send_header("Content-Length", str(len(body)))
        if body:
            self.send_header("Content-Type", "application/xml")
        self.end_headers()
        self.wfile.write(body)

    def do_PUT(self):
        body = self.read_body()
        name = self.path.split("?")[0].rstrip("/").split("/")[-1]
        out = Path(sys.argv[1])
        (out / (name + ".bin")).write_bytes(body)
        with (out / (name + ".json")).open("w") as f:
            json.dump(
                {
                    "path": self.path,
                    "headers": {k.lower(): v for k, v in self.headers.items()},
                },
                f,
                indent=2,
            )
        self.reply()

    def do_GET(self):
        self.reply(b'<?xml version="1.0" encoding="UTF-8"?><LocationConstraint/>')

    def do_HEAD(self):
        self.reply()

    def log_message(self, *args):
        pass


def main():
    out, http_port, https_port, cert, key = sys.argv[1:6]
    Path(out).mkdir(parents=True, exist_ok=True)
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", int(http_port)), Handler)
    secure = http.server.ThreadingHTTPServer(("127.0.0.1", int(https_port)), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    threading.Thread(target=plain.serve_forever, daemon=True).start()
    print("ready", flush=True)
    secure.serve_forever()


if __name__ == "__main__":
    main()
