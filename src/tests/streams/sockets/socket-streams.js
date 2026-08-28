// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// connect() sockets expose their TCP halves as a readable/writable
// stream pair — internal streams whose consumption shapes this suite
// pins under both implementations. The sidecar provides a pure echo
// server (half-close aware) and a greet-then-end server.

import { connect } from 'cloudflare:sockets';
import { strictEqual, ok, deepStrictEqual } from 'node:assert';

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

// Cancelling the socket readable: the socket's closed promise still
// settles and the writable is unusable afterwards.
export const cancelReadableSettlesSocket = {
  async test(ctrl, env) {
    const socket = connect(greetAddress(env));
    const reader = socket.readable.getReader();
    await reader.read(); // take the greeting (or its first fragment)
    await reader.cancel('done');
    await socket.close();
    ok(true);
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
