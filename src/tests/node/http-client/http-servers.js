// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Sidecar HTTP server for the node:http client suite. Reports its port as
// HTTP_SERVER_PORT=PORT on stdout for the wd_test supervisor. One server,
// routed by path:
//
// /pong            consumes the request body, then 200 "pong".
// /asd             200 "asd" at once, without reading the request.
// /echo            echoes the request body; the request's method,
//                  Content-Type, Content-Length and Transfer-Encoding come
//                  back as X-Request-* headers.
// /sink            consumes the request body; replies with a JSON summary
//                  { method, bytes, contentType, contentLength,
//                  transferEncoding }.
// /chunked?n=N&delay=MS
//                  200, then N chunks "chunk-<i>|" MS ms apart, then end.
// /large?bytes=N   200 with N bytes, byte i being i % 251, in 64 KiB writes.
// /status/CODE     CODE with a "X-Status: CODE" header and no body.
// /gzip            200 with a gzip-compressed body and Content-Encoding.
// /error-mid-body  200, "partial", then the connection is destroyed.
// /never-ends?id=ID
//                  200, "first", then holds the response open; the
//                  connection's close is recorded under ID.
// /stats?id=ID     JSON { opened, closed } for the /never-ends request ID.
// /slow-headers?delay=MS
//                  waits MS ms, then 200 "late".
// /slow-body?delay=MS
//                  200, "first", waits MS ms, then "last" and end.
//
// A second, raw TCP server (HTTP_RAW_PORT) answers with hand-written bytes
// chosen by the request path, for responses a real server would not
// produce. Each reply's segments are written 20 ms apart (the headers
// first, so the response is delivered before what follows), then the
// connection closes; the headers say `Connection: close` so that the
// runtime does not reuse the connection for the next request:
//
// /short-body      Content-Length: 10, then only "short" (5 bytes).
// /long-body       Content-Length: 4, then "longer body".
// /bad-chunked     Transfer-Encoding: chunked, then one good chunk "ok",
//                  then "zz" where a chunk size belongs.
// /empty-reply     nothing at all.
// /garbage         "this is not http\r\n\r\n".

import http from 'node:http';
import net from 'node:net';
import { gzipSync } from 'node:zlib';

const host = process.env.SIDECAR_HOSTNAME ?? '127.0.0.1';

const neverEnds = new Map();

function summary(req, bytes) {
  return {
    method: req.method,
    bytes,
    contentType: req.headers['content-type'] ?? null,
    contentLength: req.headers['content-length'] ?? null,
    transferEncoding: req.headers['transfer-encoding'] ?? null,
  };
}

function consume(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', (chunk) => chunks.push(chunk));
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const param = (name, fallback) =>
    Number(url.searchParams.get(name) ?? fallback);
  switch (true) {
    case url.pathname === '/pong': {
      await consume(req);
      res.writeHead(200);
      res.end('pong');
      break;
    }
    case url.pathname === '/asd': {
      res.end('asd');
      break;
    }
    case url.pathname === '/echo': {
      const body = await consume(req);
      const { contentType, contentLength, transferEncoding } = summary(req, 0);
      res.writeHead(200, {
        'Content-Type': contentType ?? 'application/octet-stream',
        'X-Request-Method': req.method,
        'X-Request-Content-Type': contentType ?? 'none',
        'X-Request-Content-Length': contentLength ?? 'none',
        'X-Request-Transfer-Encoding': transferEncoding ?? 'none',
      });
      res.end(body);
      break;
    }
    case url.pathname === '/sink': {
      const body = await consume(req);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(summary(req, body.length)));
      break;
    }
    case url.pathname === '/chunked': {
      const n = param('n', 3);
      const delay = param('delay', 10);
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      for (let i = 0; i < n; i++) {
        res.write(`chunk-${i}|`);
        await new Promise((r) => setTimeout(r, delay));
      }
      res.end();
      break;
    }
    case url.pathname === '/large': {
      const bytes = param('bytes', 1024 * 1024);
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': String(bytes),
      });
      let offset = 0;
      const write = () => {
        while (offset < bytes) {
          const len = Math.min(64 * 1024, bytes - offset);
          const chunk = Buffer.alloc(len);
          for (let i = 0; i < len; i++) chunk[i] = (offset + i) % 251;
          offset += len;
          if (!res.write(chunk)) {
            res.once('drain', write);
            return;
          }
        }
        res.end();
      };
      write();
      break;
    }
    case url.pathname.startsWith('/status/'): {
      const code = Number(url.pathname.slice('/status/'.length));
      res.writeHead(code, { 'X-Status': String(code) });
      res.end();
      break;
    }
    case url.pathname === '/gzip': {
      res.writeHead(200, {
        'Content-Encoding': 'gzip',
        'Content-Type': 'text/plain',
      });
      res.end(gzipSync(Buffer.from('hello from gzip server')));
      break;
    }
    case url.pathname === '/error-mid-body': {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.write('partial');
      setTimeout(() => res.destroy(), 20);
      break;
    }
    case url.pathname === '/never-ends': {
      const id = url.searchParams.get('id');
      const state = { opened: true, closed: false };
      neverEnds.set(id, state);
      res.on('close', () => {
        state.closed = true;
      });
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.write('first');
      break;
    }
    case url.pathname === '/stats': {
      const state = neverEnds.get(url.searchParams.get('id')) ?? {
        opened: false,
        closed: false,
      };
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(state));
      break;
    }
    case url.pathname === '/slow-headers': {
      await new Promise((r) => setTimeout(r, param('delay', 200)));
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('late');
      break;
    }
    case url.pathname === '/slow-body': {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.write('first');
      await new Promise((r) => setTimeout(r, param('delay', 200)));
      res.end('last');
      break;
    }
    default: {
      res.writeHead(404);
      res.end('no such route');
    }
  }
});

server.listen({ port: 0, host }, () => {
  console.log(`HTTP_SERVER_PORT=${server.address().port}`);
});

const rawReplies = {
  '/short-body': [
    'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 10\r\n\r\n',
    'short',
  ],
  '/long-body': [
    'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 4\r\n\r\n',
    'longer body',
  ],
  '/bad-chunked': [
    'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n',
    '2\r\nok\r\n',
    'zz\r\n',
  ],
  '/empty-reply': [],
  '/garbage': ['this is not http\r\n\r\n'],
};

const raw = net.createServer((socket) => {
  socket.on('error', () => {});
  let head = '';
  let replying = false;
  socket.on('data', (data) => {
    if (replying) return;
    head += data.toString('latin1');
    if (!head.includes('\r\n\r\n')) return;
    replying = true;
    const path = head.split(' ')[1] ?? '';
    const segments = rawReplies[path] ?? rawReplies['/garbage'];
    let i = 0;
    const step = () => {
      if (socket.destroyed) return;
      if (i === segments.length) {
        socket.end();
        return;
      }
      socket.write(segments[i++]);
      setTimeout(step, 20);
    };
    setTimeout(step, 20);
  });
});

raw.listen({ port: 0, host }, () => {
  console.log(`HTTP_RAW_PORT=${raw.address().port}`);
});
