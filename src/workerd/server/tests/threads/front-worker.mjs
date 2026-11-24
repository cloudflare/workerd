// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Worker behind every socket of `front-config.capnp`. Each thread runs its own copy, so
// global state is per thread; what the driver must read wherever it is written goes through the
// `Ledger` object, which lives on the first thread.
import { DurableObject } from 'cloudflare:workers';

const MiB = 1024 * 1024;
const encoder = new TextEncoder();

// The identity of this isolate: global state, so each copy of the Worker has its own.
let isolate;

function isolateId() {
  isolate ??= crypto.randomUUID();
  return isolate;
}

// The markers of the requests this isolate has started, in the order it started them.
const seen = [];

// What this isolate observed of the request with a marker, by marker.
const notes = new Map();

function note(marker, what) {
  if (!notes.has(marker)) notes.set(marker, []);
  notes.get(marker).push(what);
}

// Bytes in a repeating pattern: byte `i` of a body is `i % 251`, so a reader can check any range.
const PATTERN = Uint8Array.from(
  { length: 251 + 256 * 1024 },
  (_, i) => i % 251
);

function patternAt(offset, length) {
  const start = offset % 251;
  return PATTERN.slice(start, start + length);
}

function ledger(env, name = 'ledger') {
  return env.LEDGER.get(env.LEDGER.idFromName(name));
}

// Values any thread can read, and the hibernating WebSockets of `/room`.
export class Ledger extends DurableObject {
  #values = new Map();

  where() {
    return isolateId();
  }

  set(key, value) {
    this.#values.set(key, value);
  }

  get(key) {
    return this.#values.get(key) ?? null;
  }

  fetch() {
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    ws.send(
      JSON.stringify({
        echo: message,
        isolate: isolateId(),
        sockets: this.ctx.getWebSockets().length,
      })
    );
  }

  webSocketClose(ws, code, reason) {
    ws.close(code, reason);
  }
}

// A response whose head goes out at once and whose body is the JSON of what `produce()` resolves
// to, once it has.
function later(produce, headers) {
  const body = new ReadableStream({
    async pull(controller) {
      controller.enqueue(encoder.encode(JSON.stringify(await produce())));
      controller.close();
    },
  });
  return new Response(body, { headers });
}

export default {
  async fetch(request, env) {
    const front = isolateId();
    // A request for another isolate is refused before anything else is done with it (its body
    // is not read, an upgrade not accepted), so that the client can send it again until the
    // isolate it names serves it. The refusal is held for a while: a thread with a request in
    // flight is the last the front wakes for the next one, so the client's other attempts at
    // once go to other threads.
    const wanted = request.headers.get('x-thread');
    if (wanted !== null && wanted !== front) {
      await scheduler.wait(100);
      return Response.json(
        { front },
        { status: 409, headers: { 'x-front': front } }
      );
    }
    const url = new URL(request.url);
    const query = url.searchParams;
    const marker = query.get('marker');
    if (marker !== null) seen.push(marker);
    const headers = { 'x-front': front };

    switch (url.pathname) {
      case '/':
        return Response.json({ front }, { headers });

      case '/where':
        // Answers after `ms` milliseconds, if given, so that requests sent together are in
        // flight together, each on a thread of its own.
        await scheduler.wait(Number(query.get('ms') ?? 0));
        return Response.json(
          { front, first: await ledger(env).where() },
          { headers }
        );

      case '/seen':
        return Response.json({ front, seen }, { headers });

      case '/notes':
        return Response.json(
          { front, notes: notes.get(query.get('of')) ?? [] },
          { headers }
        );

      case '/ledger':
        return Response.json(
          { front, value: await ledger(env).get(query.get('key')) },
          { headers }
        );

      case '/hold': {
        // The head goes out at once and the body ends after `ms` milliseconds: until then the
        // request is in flight on this thread.
        const ms = Number(query.get('ms'));
        const body = new ReadableStream(
          {
            async pull(controller) {
              await scheduler.wait(ms);
              controller.close();
            },
          },
          { highWaterMark: 0 }
        );
        return new Response(body, { headers });
      }

      case '/read':
        // Reads the whole request body before it answers.
        return new Response(await request.text(), { headers });

      case '/spin': {
        // The head goes out first; then the body's first pull keeps this thread in JavaScript
        // for `ms` milliseconds, during which it serves nothing else.
        const ms = Number(query.get('ms'));
        const body = new ReadableStream(
          {
            pull(controller) {
              const start = Date.now();
              while (Date.now() < start + ms) {}
              const end = Date.now();
              controller.enqueue(
                encoder.encode(JSON.stringify({ front, marker, start, end }))
              );
              controller.close();
            },
          },
          { highWaterMark: 0 }
        );
        return new Response(body, { headers });
      }

      case '/sleep': {
        // Answers after `ms` milliseconds; with `announce`, it first tells the ledger that it
        // arrived.
        if (query.has('announce')) {
          await ledger(env).set(`arrived:${marker}`, front);
        }
        await scheduler.wait(Number(query.get('ms') ?? 0));
        return Response.json({ front, marker }, { headers });
      }

      case '/wait': {
        // Never answers: waits for the client to go away, which `request.signal` reports.
        request.signal.addEventListener('abort', () => {
          note(marker, 'aborted');
          console.log(`front-test: ${marker} aborted`);
        });
        await ledger(env).set(`arrived:${marker}`, front);
        for (let i = 0; i < 600 && !request.signal.aborted; i++) {
          await scheduler.wait(100);
        }
        return Response.json({ front }, { headers });
      }

      case '/upload': {
        // Reads the request body, the first `slow` bytes of it at `rate` bytes a second, and
        // tells the ledger how much it has read, every MiB while it reads slowly. The response
        // head goes out at once; its body says how much was read, once the request body ended.
        // With `work`, it runs JavaScript for that many milliseconds per MiB it reads and waits
        // for nothing but the body, so that the body arrives faster than it is read.
        const slow = Number(query.get('slow') ?? 0);
        const rate = Number(query.get('rate') ?? 1);
        const work = Number(query.get('work') ?? 0);
        const reader = request.body.getReader();
        const consumed = (async () => {
          const start = Date.now();
          let total = 0;
          let reported = 0;
          for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            total += value.byteLength;
            while (Date.now() < start + (total / MiB) * work) {}
            if (total <= slow) {
              const wait = start + (total / rate) * 1000 - Date.now();
              if (wait > 0) await scheduler.wait(wait);
              if (total - reported >= MiB) {
                reported = total;
                await ledger(env).set(marker, total);
              }
            }
          }
          await ledger(env).set(marker, total);
          return { front, consumed: total };
        })();
        return later(() => consumed, headers);
      }

      case '/download': {
        // A body of `size` bytes in the pattern, made a chunk at a time as the response asks for
        // it; the ledger knows how much has been made, to the MiB.
        const size = Number(query.get('size'));
        let produced = 0;
        let reported = 0;
        const body = new ReadableStream(
          {
            async pull(controller) {
              if (produced >= size) {
                controller.close();
                return;
              }
              const length = Math.min(64 * 1024, size - produced);
              const chunk = patternAt(produced, length);
              produced += length;
              if (produced - reported >= MiB || produced === size) {
                reported = produced;
                await ledger(env).set(marker, produced);
              }
              controller.enqueue(chunk);
            },
          },
          { highWaterMark: 0 }
        );
        return new Response(body, { headers });
      }

      case '/echo':
        return new Response(request.body, { headers });

      case '/ignore':
        // Answers without reading the request body.
        return Response.json({ front }, { headers });

      case '/throw':
        throw new Error(`front-test: ${marker} threw before the head`);

      case '/fail-after-head': {
        // Sends four KiB of the body, then fails it; `framing=length` declares the length of
        // eight first.
        const fail = new Error(`front-test: ${marker} failed after the head`);
        if (query.get('framing') === 'length') {
          const { readable, writable } = new FixedLengthStream(8 * 1024);
          const writer = writable.getWriter();
          (async () => {
            for (let i = 0; i < 4; i++) {
              await writer.write(new Uint8Array(1024).fill(97));
            }
            await writer.abort(fail);
          })().catch(() => {});
          return new Response(readable, { headers });
        }
        let sent = 0;
        const body = new ReadableStream({
          pull(controller) {
            if (sent++ < 4) {
              controller.enqueue(new Uint8Array(1024).fill(97));
            } else {
              controller.error(fail);
            }
          },
        });
        return new Response(body, { headers });
      }

      case '/stream-echo': {
        // Echoes the request body a chunk at a time and notes how its streams end.
        const reader = request.body.getReader();
        request.signal.addEventListener('abort', () => {
          note(marker, 'signal aborted');
        });
        const body = new ReadableStream(
          {
            async pull(controller) {
              let result;
              try {
                result = await reader.read();
              } catch (error) {
                note(marker, `request body failed: ${error.message}`);
                throw error;
              }
              if (result.done) {
                note(marker, 'request body ended');
                controller.close();
              } else {
                controller.enqueue(result.value);
              }
            },
            cancel(reason) {
              note(marker, `response cancelled: ${reason}`);
            },
          },
          { highWaterMark: 0 }
        );
        return new Response(body, { headers });
      }

      case '/ws': {
        // Echoes every message; `close-me` has the Worker close the WebSocket instead. The
        // close the client sends is noted and returned.
        const [client, server] = Object.values(new WebSocketPair());
        server.accept();
        server.addEventListener('message', (event) => {
          if (event.data === 'close-me') {
            server.close(4001, 'bye from the worker');
          } else {
            server.send(event.data);
          }
        });
        server.addEventListener('close', (event) => {
          note(marker, { code: event.code, reason: event.reason });
          try {
            server.close(event.code, event.reason);
          } catch {
            // The Worker closed it first.
          }
        });
        return new Response(null, { status: 101, webSocket: client, headers });
      }

      case '/room': {
        // A WebSocket the object `name` hibernates with.
        const response = await ledger(env, query.get('name')).fetch(request);
        return new Response(null, {
          status: 101,
          webSocket: response.webSocket,
          headers,
        });
      }

      case '/meta':
        return Response.json(
          {
            front,
            method: request.method,
            url: request.url,
            headers: Object.fromEntries(request.headers),
            cf: request.cf ?? null,
          },
          { headers }
        );
    }
    return new Response('not found', { status: 404, headers });
  },

  // Greets with this isolate's identity, echoes what arrives, and once the client has finished
  // sending says how many bytes it got and closes.
  async connect(socket) {
    const writer = socket.writable.getWriter();
    await writer.write(encoder.encode(`hello ${isolateId()}\n`));
    let received = 0;
    for await (const chunk of socket.readable) {
      received += chunk.byteLength;
      await writer.write(chunk);
    }
    await writer.write(encoder.encode(`bye ${received}\n`));
    await writer.close();
  },
};
