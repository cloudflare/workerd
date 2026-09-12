// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without streams_enable_constructors the C++ implementation refuses
// `new ReadableStream()`, and the ServerResponse, which builds its body
// that way when the headers go out, cannot send one: the first body write
// throws the constructor-gate Error. Everything that constructs no stream
// keeps working — bodiless responses (204, HEAD), and the request body,
// which is a stream the runtime provides.

import { strictEqual, deepStrictEqual, rejects, throws } from 'node:assert';
import { Buffer } from 'node:buffer';
import { withServer } from 'harness';

const gateError = {
  name: 'Error',
  message:
    /^To use the new ReadableStream\(\) constructor, enable the streams_enable_constructors compatibility flag\./,
};

// writeHead() only formats the header; the first write() sends it and hits
// the gate, synchronously, in the handler. A handler that catches the
// Error and ends the response anyway never produces a Response: the fetch
// fails with a premature close.
export const legacyFirstBodyWriteHitsConstructorGate = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.writeHead(200, { 'X-Legacy': 'yes' });
        throws(() => res.write('body'), gateError);
        events.push('write threw');
        res.end();
        events.push(`destroyed:${res.destroyed}`);
      },
      async () => {
        await rejects(env.SERVICE.fetch('http://x/'), {
          name: 'TypeError',
          message: 'Premature close',
        });
        deepStrictEqual(events, ['write threw', 'destroyed:false']);
      }
    );
  },
};

// Left uncaught, the gate Error escaping the handler fails the fetch with
// that Error.
export const legacyUncaughtGateErrorFailsFetch = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.end('body');
      },
      async () => {
        await rejects(env.SERVICE.fetch('http://x/'), gateError);
      }
    );
  },
};

// Responses without a body — a status that forbids one (204, 304), or the
// reply to a HEAD — construct no stream: they carry their status and
// headers with a null body, and the writes the handler still issues are
// dropped.
export const legacyBodilessResponsesWork = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(Number(req.url.slice(1)), { 'X-Path': req.url });
        res.write('dropped');
        res.end('dropped too');
      },
      async () => {
        for (const [status, method] of [
          [204, 'GET'],
          [304, 'GET'],
          [200, 'HEAD'],
        ]) {
          const res = await env.SERVICE.fetch(`http://x/${status}`, { method });
          strictEqual(res.status, status);
          strictEqual(res.headers.get('X-Path'), `/${status}`);
          strictEqual(res.body, null);
        }
      }
    );
  },
};

// The request body is the runtime's stream: the IncomingMessage pumps it
// as usual, reported here through the headers of a bodiless reply.
export const legacyRequestBodyIsPumped = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        const chunks = [];
        req.on('data', (chunk) => chunks.push(chunk));
        req.on('end', () => {
          res.writeHead(204, {
            'X-Body': Buffer.concat(chunks).toString(),
            'X-Complete': String(req.complete),
          });
          res.end();
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: 'payload',
        });
        strictEqual(res.status, 204);
        strictEqual(res.headers.get('X-Body'), 'payload');
        strictEqual(res.headers.get('X-Complete'), 'true');
      }
    );
  },
};
