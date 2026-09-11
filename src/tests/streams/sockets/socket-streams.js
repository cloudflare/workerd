// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// connect() sockets expose their TCP halves as a readable/writable
// stream pair — internal streams whose consumption shapes this suite
// pins under both implementations. The sidecar provides a pure echo
// server (half-close aware) and a greet-then-end server.
//
// Canceling a pending read diverges: C++ rejects with a re-created Error
// carrying the cancel reason, while TypeScript resolves the read done.

import { connect } from 'cloudflare:sockets';
import { strictEqual, ok, rejects, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const dec = new TextDecoder();

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

function echoAddress(env) {
  return `${env.SIDECAR_HOSTNAME}:${env.STREAMS_ECHO_PORT}`;
}

function greetAddress(env) {
  return `${env.SIDECAR_HOSTNAME}:${env.STREAMS_GREET_PORT}`;
}

async function drainToBytes(readable) {
  const reader = readable.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    parts.push(value);
    total += value.byteLength;
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  reader.releaseLock();
  return out;
}

// Write, half-close, and read the echo back to EOF with a default
// reader.
export const echoRoundTrip = {
  async test(ctrl, env) {
    const socket = connect(echoAddress(env));
    const writer = socket.writable.getWriter();
    await writer.write(enc.encode('hello '));
    await writer.write(enc.encode('sockets'));
    await writer.close(); // half-close: server flushes and ends
    const echoed = await drainToBytes(socket.readable);
    strictEqual(dec.decode(echoed), 'hello sockets');
    await socket.close();
  },
};

// The greet server ends after one message: the readable delivers it
// and reaches done; the socket's closed promise settles.
export const greetReadsToEof = {
  async test(ctrl, env) {
    const socket = connect(greetAddress(env));
    const bytes = await drainToBytes(socket.readable);
    strictEqual(dec.decode(bytes), 'hello from the greet server');
    const reader = socket.readable.getReader();
    const tail = await reader.read();
    strictEqual(tail.done, true);
    strictEqual(tail.value, undefined);
    await socket.close();
  },
};

// BYOB reads over the socket readable, byte-exact against the echo.
export const echoByobReads = {
  async test(ctrl, env) {
    const socket = connect(echoAddress(env));
    const writer = socket.writable.getWriter();
    const payload = patternChunk(0, 4096);
    await writer.write(payload);
    await writer.close();
    const reader = socket.readable.getReader({ mode: 'byob' });
    const out = new Uint8Array(4096);
    let offset = 0;
    let view = new Uint8Array(1024);
    for (;;) {
      const { value, done } = await reader.read(view);
      if (done) break;
      out.set(value, offset);
      offset += value.byteLength;
      view = new Uint8Array(value.buffer);
    }
    strictEqual(offset, 4096);
    deepStrictEqual(out, payload);
    await socket.close();
  },
};

// readAtLeast accumulates echo fragments up to the minimum.
export const echoReadAtLeast = {
  async test(ctrl, env) {
    const socket = connect(echoAddress(env));
    const writer = socket.writable.getWriter();
    // Three separate writes; readAtLeast(12) must accumulate across
    // however TCP fragments them.
    await writer.write(enc.encode('aaaa'));
    await writer.write(enc.encode('bbbb'));
    await writer.write(enc.encode('cccc'));
    await writer.close();
    const reader = socket.readable.getReader({ mode: 'byob' });
    const first = await reader.readAtLeast(12, new Uint8Array(64));
    ok(first.value.byteLength >= 12);
    strictEqual(
      dec.decode(first.value),
      'aaaabbbbcccc'.slice(0, first.value.byteLength)
    );
    await socket.close();
  },
};

// Socket readable piped into a JS sink.
export const pipeSocketReadableToJsSink = {
  async test(ctrl, env) {
    const socket = connect(greetAddress(env));
    const chunks = [];
    await socket.readable.pipeTo(
      new WritableStream({
        write(chunk) {
          chunks.push(chunk);
        },
      })
    );
    const total = chunks.reduce((n, c) => n + c.byteLength, 0);
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    strictEqual(dec.decode(bytes), 'hello from the greet server');
    await socket.close();
  },
};

// A JS source piped into the socket writable, echo read back
// concurrently.
export const pipeJsSourceToSocketWritable = {
  async test(ctrl, env) {
    const socket = connect(echoAddress(env));
    const source = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('piped '));
        c.enqueue(enc.encode('payload'));
        c.close();
      },
    });
    const [echoed] = await Promise.all([
      drainToBytes(socket.readable),
      source.pipeTo(socket.writable),
    ]);
    strictEqual(dec.decode(echoed), 'piped payload');
    await socket.close();
  },
};

// Socket readable piped THROUGH a JS transform.
export const pipeSocketThroughJsTransform = {
  async test(ctrl, env) {
    const socket = connect(greetAddress(env));
    const upper = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(dec.decode(chunk, { stream: true }).toUpperCase());
      },
    });
    const parts = [];
    await socket.readable.pipeThrough(upper).pipeTo(
      new WritableStream({
        write(chunk) {
          parts.push(chunk);
        },
      })
    );
    strictEqual(parts.join(''), 'HELLO FROM THE GREET SERVER');
    await socket.close();
  },
};

// Socket-to-socket: the greet server's output piped into the echo
// server, echo read back.
export const pipeSocketToSocket = {
  async test(ctrl, env) {
    const greetSocket = connect(greetAddress(env));
    const echoSocket = connect(echoAddress(env));
    const [echoed] = await Promise.all([
      drainToBytes(echoSocket.readable),
      greetSocket.readable.pipeTo(echoSocket.writable),
    ]);
    strictEqual(dec.decode(echoed), 'hello from the greet server');
    await Promise.all([greetSocket.close(), echoSocket.close()]);
  },
};

// Cancel a pending socket read while the peer remains open, then close
// the socket and observe its public closed promise.
export const cancelReadableSettlesSocket = {
  async test(ctrl, env) {
    const socket = connect(echoAddress(env));
    await socket.opened;
    const reader = socket.readable.getReader();
    const pendingRead = reader.read();
    await reader.cancel('done');
    if (usingTsImpl) {
      const { value, done } = await pendingRead;
      strictEqual(done, true);
      strictEqual(value, undefined);
    } else {
      await rejects(pendingRead, { name: 'Error', message: 'done' });
    }
    await socket.close();
    await socket.closed;
  },
};

// VOLUME: 256 KiB patterned bytes through the echo, concurrent
// producer/consumer, byte-exact.
export const largeEchoVolume = {
  async test(ctrl, env) {
    const TOTAL = 256 * 1024;
    const CHUNK = 16 * 1024;
    const socket = connect(echoAddress(env));
    const producer = (async () => {
      const writer = socket.writable.getWriter();
      for (let offset = 0; offset < TOTAL; offset += CHUNK) {
        await writer.write(patternChunk(offset, CHUNK));
      }
      await writer.close();
    })();
    const consumer = (async () => {
      const reader = socket.readable.getReader();
      let received = 0;
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (let i = 0; i < value.byteLength; i++) {
          if (value[i] !== (received + i) % PATTERN_MODULUS) {
            strictEqual(
              value[i],
              (received + i) % PATTERN_MODULUS,
              `pattern break at byte ${received + i}`
            );
          }
        }
        received += value.byteLength;
      }
      return received;
    })();
    const [, received] = await Promise.all([producer, consumer]);
    strictEqual(received, TOTAL);
    await socket.close();
  },
};

// CLOSE WITH A PIPE CLOSE IN FLIGHT: a connect() handler pipes an inbound
// socket to an outbound one; once the pipes settle it closes both. Under the
// C++ implementation the pipe's close of the writable may still be queued at
// that point (pipeTo resolves before it completes; TypeScript resolves after),
// and socket.close() aborts it. Whatever a later writer's closed promise then
// reports must be a real error, never undefined.
export default {
  async connect(socket, env) {
    const { localAddress } = await socket.opened;
    if (localAddress.startsWith('echo:')) {
      // Explicit loop: a same-socket pipeTo over an in-process pipe splices,
      // so the peer's write could not complete before it reads the echo.
      const writer = socket.writable.getWriter();
      for await (const chunk of socket.readable) {
        await writer.write(chunk);
      }
      // The socket closes its write side itself on the peer's FIN; the
      // TypeScript implementation reports a close racing that as an error.
      await writer.close().catch(() => {});
      await socket.close();
      await socket.closed;
      return;
    }
    // Everything observed here is asserted by closeWithPipeCloseInFlight,
    // which runs in a different request.
    const outcome = {};
    globalThis.proxyOutcome = outcome;
    try {
      const target = env.SELF.connect('echo:1', { allowHalfOpen: true });
      const pipes = await Promise.allSettled([
        socket.readable.pipeTo(target.writable),
        target.readable.pipeTo(socket.writable),
      ]);
      outcome.pipes = pipes.map((r) => r.status);
      const closes = await Promise.allSettled([socket.close(), target.close()]);
      outcome.closes = closes.map((r) => r.status);
      outcome.abortReasons = [];
      for (const s of [socket, target]) {
        let writer;
        try {
          writer = s.writable.getWriter();
        } catch {
          // Still locked to the pipe (TypeScript); nothing to observe.
          continue;
        }
        outcome.abortReasons.push(
          await writer.closed.then(
            () => 'resolved',
            (e) => e
          )
        );
      }
      await Promise.all([socket.closed, target.closed]);
      outcome.closed = 'resolved';
    } catch (e) {
      outcome.error = e;
    }
  },
};

export const closeWithPipeCloseInFlight = {
  async test(ctrl, env) {
    const socket = env.SELF.connect('proxy:1');
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('hello'));
    const reader = socket.readable.getReader();
    const { value } = await reader.read();
    strictEqual(new TextDecoder().decode(value), 'hello');
    reader.releaseLock();
    await writer.close();
    strictEqual((await drainToBytes(socket.readable)).length, 0);
    // The handler runs in its own request; give its closes a moment.
    for (
      let i = 0;
      i < 100 && globalThis.proxyOutcome?.closed === undefined;
      i++
    ) {
      await scheduler.wait(10);
    }
    const outcome = globalThis.proxyOutcome;
    strictEqual(outcome.error, undefined, String(outcome.error));
    deepStrictEqual(outcome.pipes, ['fulfilled', 'fulfilled']);
    deepStrictEqual(outcome.closes, ['fulfilled', 'fulfilled']);
    strictEqual(outcome.closed, 'resolved');
    for (const reason of outcome.abortReasons) {
      // Resolved when the queued close completed before the abort took effect
      // (compat dates before internal_writable_stream_abort_clears_queue).
      if (reason === 'resolved') continue;
      ok(reason instanceof TypeError, String(reason));
      strictEqual(reason.message, 'This socket has been closed.');
    }
  },
};
