// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Cross-implementation stream transfer over JS RPC. Companion configs run this file with
// opposite implementation assignments, so streams originating from both implementations cross
// to the other implementation and back. The wire protocol is implementation-agnostic; each side
// constructs received streams with its own implementation, so received streams are always
// instanceof the receiver's own globals.

import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

const enc = new TextEncoder();
const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams'] ===
  true;

function makeConstructorBackedEchoPair() {
  let controller;
  const readable = new ReadableStream({
    start(c) {
      controller = c;
    },
  });
  const writable = new WritableStream({
    write(chunk) {
      controller.enqueue(chunk);
    },
    close() {
      controller.close();
    },
    abort(reason) {
      controller.error(reason);
    },
  });
  return { readable, writable };
}

async function isCrossImplementationRunner(env) {
  if (env.PEER === undefined) return false;
  assert.notStrictEqual(usingTsImpl, await env.PEER.usesTypeScriptStreams());
  return true;
}

async function inspectLength(stream, http) {
  const response = await http.fetch('http://length/', {
    method: 'POST',
    body: stream,
  });
  return response.json();
}

export class Peer extends WorkerEntrypoint {
  usesTypeScriptStreams() {
    return usingTsImpl;
  }

  // Receives a caller-created readable; it deserializes as this worker's implementation.
  async readFrom(stream) {
    assert.ok(stream instanceof ReadableStream);
    return await new Response(stream).text();
  }

  // Receives a runner-created writable and writes a fixed payload into it.
  async writeTo(stream) {
    assert.ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.write(enc.encode('written by the peer'));
    await writer.close();
  }

  // Aborting the received writable drops this side's reference to the RPC stream, but the call's
  // parameter message holds the stream capability until the call completes, so the origin
  // observes the disconnect only once this call returns. The abort reason is not carried on the
  // wire.
  async abortWritable(stream) {
    assert.ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.abort(new Error('aborted by the peer'));
  }

  roundTrip(value) {
    return value;
  }

  makeResponseBody(text) {
    return new Response(text).body;
  }

  inspectLength(stream) {
    return inspectLength(stream, this.env.HTTP);
  }

  // Returns a peer-created readable.
  makeReadable() {
    return new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('made by the peer'));
        c.close();
      },
    });
  }

  // Returns both halves of a peer-local identity transform, nested in an object (multiple
  // stream externals in one value graph). The runner writes into `writable` and reads the
  // echo from `readable`, so bytes traverse the wire in both directions through the peer's
  // transform.
  makeEchoPair() {
    const { readable, writable } = new IdentityTransformStream();
    return { readable, writable };
  }

  // Unlike IdentityTransformStream, both halves exercise the constructor-backed stream paths.
  makeConstructorBackedEchoPair() {
    return makeConstructorBackedEchoPair();
  }

  makeReadableErrorPair() {
    let controller;
    const readable = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const writable = new WritableStream({
      write() {
        controller.error(new Error('source failed'));
      },
    });
    return { readable, writable };
  }
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.hostname === 'body') {
      return new Response(url.searchParams.get('text'));
    }
    return Response.json({
      length: request.headers.get('Content-Length'),
      text: await request.text(),
    });
  },

  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;

    // Caller-serialized -> peer-deserialized (argument direction), readable.
    {
      const stream = new ReadableStream({
        start(c) {
          c.enqueue(enc.encode('made by the runner'));
          c.close();
        },
      });
      assert.strictEqual(await env.PEER.readFrom(stream), 'made by the runner');
    }

    // Caller-serialized -> peer-deserialized, native/system-backed writable.
    {
      const { readable, writable } = new IdentityTransformStream();
      const promise = env.PEER.writeTo(writable);
      assert.strictEqual(
        await new Response(readable).text(),
        'written by the peer'
      );
      await promise;
    }

    // Caller-serialized -> peer-deserialized, constructor-backed writable. In the legacy
    // implementation this selects the writer-driven serializer rather than the native-sink path.
    {
      const { readable, writable } = makeConstructorBackedEchoPair();
      const promise = env.PEER.writeTo(writable);
      assert.strictEqual(
        await new Response(readable).text(),
        'written by the peer'
      );
      await promise;
    }

    // Peer-serialized -> caller-deserialized (return direction), readable. The received
    // stream must be an instance of this worker's global.
    {
      const stream = await env.PEER.makeReadable();
      assert.ok(stream instanceof ReadableStream);
      assert.strictEqual(await new Response(stream).text(), 'made by the peer');
    }

    // Peer-serialized -> caller-deserialized, both directions at once through the peer's
    // native/system-backed echo pair, nested in an object.
    {
      const { readable, writable } = await env.PEER.makeEchoPair();
      assert.ok(readable instanceof ReadableStream);
      assert.ok(writable instanceof WritableStream);
      const writer = writable.getWriter();
      await writer.write(enc.encode('echoed through the peer'));
      await writer.close();
      assert.strictEqual(
        await new Response(readable).text(),
        'echoed through the peer'
      );
    }

    // Peer-serialized -> caller-deserialized, constructor-backed readable and writable.
    {
      const { readable, writable } =
        await env.PEER.makeConstructorBackedEchoPair();
      assert.ok(readable instanceof ReadableStream);
      assert.ok(writable instanceof WritableStream);
      const writer = writable.getWriter();
      await writer.write(enc.encode('constructor-backed echo'));
      await writer.close();
      assert.strictEqual(
        await new Response(readable).text(),
        'constructor-backed echo'
      );
    }

    // Retransfer both stream values: caller -> peer -> caller. Running this under both configs
    // covers both mixed adapter compositions for RPC-backed streams.
    {
      const pair = await env.PEER.roundTrip(makeConstructorBackedEchoPair());
      assert.ok(pair.readable instanceof ReadableStream);
      assert.ok(pair.writable instanceof WritableStream);
      const text = new Response(pair.readable).text();
      const writer = pair.writable.getWriter();
      await writer.write(enc.encode('round-tripped across implementations'));
      await writer.close();
      assert.strictEqual(await text, 'round-tripped across implementations');
    }
  },
};

export const readableLength = {
  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;
    // Arguments and results preserve both known zero and a UTF-8 byte count.
    for (const text of ['', 'snowman: \u2603']) {
      const expected = { length: String(enc.encode(text).byteLength), text };
      assert.deepStrictEqual(
        await env.PEER.inspectLength(new Response(text).body),
        expected
      );
      const returned = await env.PEER.makeResponseBody(text);
      assert.deepStrictEqual(await inspectLength(returned, env.HTTP), expected);
    }

    // Native HTTP bodies, a TransformStream's readable, and default and byte sources each
    // declare their length.
    const text = 'known length';
    const bytes = enc.encode(text);
    const expected = { length: String(bytes.byteLength), text };
    const valueStream = new TransformStream({
      expectedLength: bytes.byteLength,
      start(c) {
        c.enqueue(bytes);
      },
    });
    await valueStream.writable.getWriter().close();
    for (const [name, stream] of [
      [
        'http',
        (await env.HTTP.fetch(`http://body/?text=${encodeURIComponent(text)}`))
          .body,
      ],
      ['value', valueStream.readable],
      [
        'source',
        new ReadableStream({
          expectedLength: bytes.byteLength,
          start(c) {
            c.enqueue(bytes.slice());
            c.close();
          },
        }),
      ],
      [
        'bytes',
        new ReadableStream({
          type: 'bytes',
          expectedLength: bytes.byteLength,
          start(c) {
            c.enqueue(bytes.slice());
            c.close();
          },
        }),
      ],
    ]) {
      assert.deepStrictEqual(
        await env.PEER.inspectLength(stream),
        expected,
        name
      );
    }

    // Forwarding a received stream and teeing it preserve the length on both branches.
    const forwarded = await env.PEER.roundTrip(new Response(text).body);
    for (const branch of forwarded.tee()) {
      assert.deepStrictEqual(await env.PEER.inspectLength(branch), expected);
    }

    // Transfer the fixed-length stream before its producer writes any bytes.
    const { readable, writable } = new FixedLengthStream(6);
    const reading = env.PEER.inspectLength(readable);
    const writer = writable.getWriter();
    await writer.write(enc.encode('foobar'));
    await writer.close();
    assert.deepStrictEqual(await reading, { length: '6', text: 'foobar' });

    const unsized = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('unsized'));
        c.close();
      },
    });
    assert.deepStrictEqual(await env.PEER.inspectLength(unsized), {
      length: null,
      text: 'unsized',
    });

    // A stream read from before the transfer has only part of its declared total left. A
    // TypeScript origin sends it with no length. A C++ origin still declares the total, a defect
    // (the streams readable suite's ledger #36), so the body the peer forwards falls short and
    // reading it fails.
    for (const type of [undefined, 'bytes']) {
      const partlyRead = new ReadableStream({
        type,
        expectedLength: bytes.byteLength,
        start(c) {
          c.enqueue(bytes.slice(0, 5));
          c.enqueue(bytes.slice(5));
          c.close();
        },
      });
      // The byte stream is read through a BYOB reader, as a default read of a C++ byte stream
      // takes every queued chunk.
      const byob = type === 'bytes';
      const reader = partlyRead.getReader(byob ? { mode: 'byob' } : {});
      const { value } = await reader.read(byob ? new Uint8Array(5) : undefined);
      assert.strictEqual(new TextDecoder().decode(value), 'known');
      reader.releaseLock();
      const inspected = env.PEER.inspectLength(partlyRead);
      if (usingTsImpl) {
        assert.deepStrictEqual(
          await inspected,
          { length: null, text: ' length' },
          String(type)
        );
      } else {
        await assert.rejects(
          inspected,
          { message: 'Network connection lost.' },
          String(type)
        );
      }
    }
  },
};

export const writableAbortDisconnectsOrigin = {
  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;

    // Abort reasons are not carried by the byte-stream protocol, so the origin's adapter reports
    // its generic disconnect text when the peer's call releases the stream (see
    // Peer.abortWritable).
    let abortCalls = 0;
    const { promise, resolve } = Promise.withResolvers();
    const writable = new WritableStream({
      abort(reason) {
        ++abortCalls;
        resolve(reason);
      },
    });
    await env.PEER.abortWritable(writable);
    const reason = await promise;
    assert.strictEqual(abortCalls, 1);
    assert.strictEqual(reason.name, 'Error');
    assert.strictEqual(
      reason.message,
      'WritableStream received over RPC was disconnected because the remote execution ' +
        'context has ended.'
    );
  },
};

export const constructorReadableErrorDisconnectsPeer = {
  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;

    let sourceController;
    const readable = new ReadableStream({
      start(c) {
        sourceController = c;
      },
    });
    const read = env.PEER.readFrom(readable);
    sourceController.enqueue(enc.encode('partial'));
    sourceController.error(new Error('source failed'));
    await assert.rejects(read, {
      name: 'Error',
      message: 'ReadableStream received over RPC disconnected prematurely.',
    });
  },
};

export const returnedReadableErrorDisconnectsCaller = {
  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;

    const { readable, writable } = await env.PEER.makeReadableErrorPair();
    assert.ok(readable instanceof ReadableStream);
    assert.ok(writable instanceof WritableStream);
    const read = new Response(readable).text();
    const writer = writable.getWriter();
    await writer.write(enc.encode('trigger source error'));
    await writer.close();
    // Result streams surface different transport errors in the two receiving implementations.
    await assert.rejects(read, {
      name: 'Error',
      message: usingTsImpl
        ? 'ReadableStream received over RPC disconnected prematurely.'
        : 'Network connection lost.',
    });
  },
};

export const nativeReadableAbortDisconnectsPeer = {
  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;

    const { readable, writable } = new IdentityTransformStream();
    const read = env.PEER.readFrom(readable);
    const writer = writable.getWriter();
    await writer.write(enc.encode('partial'));
    await writer.abort(new Error('producer failed'));
    await assert.rejects(read, {
      name: 'Error',
      message: 'ReadableStream received over RPC disconnected prematurely.',
    });
  },
};

export const lockedStreamsRejected = {
  async test(controller, env) {
    if (!(await isCrossImplementationRunner(env))) return;

    const { readable, writable } = makeConstructorBackedEchoPair();
    const reader = readable.getReader();
    const writer = writable.getWriter();
    const write = writer.write(enc.encode('still usable'));

    await assert.rejects(async () => env.PEER.writeTo(writable), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot get a writer for a stream that is locked'
        : 'This WritableStream is currently locked to a writer.',
    });
    await assert.rejects(async () => env.PEER.readFrom(readable), {
      name: 'TypeError',
      message: 'The ReadableStream has been locked to a reader.',
    });

    const result = await reader.read();
    assert.strictEqual(new TextDecoder().decode(result.value), 'still usable');
    await write;
    await writer.close();
    assert.strictEqual((await reader.read()).done, true);
  },
};
