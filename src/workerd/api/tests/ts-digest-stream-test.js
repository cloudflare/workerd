// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests specific to the TypeScript DigestStream. Behavior shared with the C++
// implementation is covered by crypto-streams-test.js, which both
// implementations run unmodified; this file covers what only the TypeScript
// version can do, plus the edge cases neither had coverage for.

import { strictEqual, deepStrictEqual, ok, rejects, throws } from 'node:assert';

// The reason this implementation exists: with typescript_implemented_streams
// enabled, the C++ DigestStream inherits from the C++ WritableStream, which is
// no longer the WritableStream user code sees. These assertions fail against
// the C++ implementation, so they double as proof that the swap took effect —
// without them the rest of this file would pass either way.
export const isRealWritableStreamSubclass = {
  test() {
    const stream = new crypto.DigestStream('md5');
    ok(
      stream instanceof WritableStream,
      'DigestStream instance must be a WritableStream'
    );
    strictEqual(
      Object.getPrototypeOf(crypto.DigestStream),
      WritableStream,
      'DigestStream must extend the global WritableStream'
    );
    strictEqual(
      Object.getPrototypeOf(crypto.DigestStream.prototype),
      WritableStream.prototype
    );
    // Inherited members must be reachable, not shadowed.
    strictEqual(typeof stream.getWriter, 'function');
    strictEqual(stream.locked, false);
  },
};

// The user-visible consequence of the above: piping into a DigestStream.
// pipeTo checks its destination with a brand check, so a foreign WritableStream
// is rejected outright.
export const pipeToWorks = {
  async test() {
    const check = new Uint8Array([
      93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
    ]);
    const enc = new TextEncoder();
    const digestStream = new crypto.DigestStream('md5');

    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('hello'));
        controller.close();
      },
    });

    await source.pipeTo(digestStream);
    deepStrictEqual(new Uint8Array(await digestStream.digest), check);
    strictEqual(digestStream.bytesWritten, 5n);
  },
};

export const pipeThroughIntoDigest = {
  async test() {
    const enc = new TextEncoder();
    const digestStream = new crypto.DigestStream('crc32');
    const { readable, writable } = new TransformStream();

    const writer = writable.getWriter();
    const pipe = readable.pipeTo(digestStream);
    await writer.write(enc.encode('Hello world'));
    await writer.close();
    await pipe;

    // Same expectation as the shared AWS CRC vector for 'Hello world'.
    deepStrictEqual(
      new Uint8Array(await digestStream.digest),
      new Uint8Array([0x8b, 0xd6, 0x9e, 0x52])
    );
  },
};

export const digestPromiseIdentityIsStable = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const first = stream.digest;
    const second = stream.digest;
    strictEqual(first, second, 'digest must return the same promise object');

    const writer = stream.getWriter();
    await writer.close();
    await first;
    // Still the same object after settling.
    strictEqual(stream.digest, first);
  },
};

export const bytesWrittenIsBigInt = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    strictEqual(typeof stream.bytesWritten, 'bigint');
    strictEqual(stream.bytesWritten, 0n);

    const writer = stream.getWriter();
    await writer.write(new Uint8Array(3));
    strictEqual(stream.bytesWritten, 3n);
    // A zero-length write must not move the counter.
    await writer.write(new Uint8Array(0));
    strictEqual(stream.bytesWritten, 3n);
    // Strings count their UTF-8 length, not their UTF-16 length.
    await writer.write('\u00e9');
    strictEqual(stream.bytesWritten, 5n);
    await writer.close();
  },
};

export const toStringTag = {
  test() {
    const stream = new crypto.DigestStream('md5');
    strictEqual(
      Object.prototype.toString.call(stream),
      '[object DigestStream]'
    );
  },
};

export const rejectsNonByteChunks = {
  async test() {
    const expected =
      'DigestStream is a byte stream but received an object ' +
      'of non-ArrayBuffer/ArrayBufferView/string type on its writable side.';

    for (const bad of [123, null, undefined, {}, [], true, Symbol.iterator]) {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      await rejects(writer.write(bad), { message: expected });
    }
  },
};

// A bare SharedArrayBuffer is not an ArrayBuffer as far as V8 is concerned, so
// it is rejected; a view onto one is accepted. This split is inherited from the
// C++ implementation and was previously untested.
export const sharedArrayBufferHandling = {
  async test() {
    if (typeof SharedArrayBuffer === 'undefined') return;

    {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      await rejects(writer.write(new SharedArrayBuffer(8)), {
        message:
          'DigestStream is a byte stream but received an object ' +
          'of non-ArrayBuffer/ArrayBufferView/string type on its writable side.',
      });
    }

    {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      await writer.write(new Uint8Array(new SharedArrayBuffer(4)));
      await writer.close();
      await stream.digest;
      strictEqual(stream.bytesWritten, 4n);
    }
  },
};

export const unknownAlgorithmThrowsSynchronously = {
  test() {
    throws(() => new crypto.DigestStream('foo'));
    throws(() => new crypto.DigestStream(''));
    // A missing name coerces to "undefined" and then fails the lookup.
    throws(() => new crypto.DigestStream({}));
    throws(() => new crypto.DigestStream(null));
    throws(() => new crypto.DigestStream(undefined));
  },
};

export const algorithmAsObject = {
  async test() {
    const check = new Uint8Array([
      93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
    ]);
    const stream = new crypto.DigestStream({ name: 'md5' });
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    deepStrictEqual(new Uint8Array(await stream.digest), check);
  },
};

// dispose() errors the digest without touching the WritableStream, so a
// zero-length write still resolves: the sink short-circuits on length before it
// ever looks at the digest state. Non-empty writes reject. Neither ordering was
// previously covered.
export const disposeThenZeroLengthWrite = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();

    // The stream itself is untouched by dispose.
    strictEqual(stream.locked, false);

    const writer = stream.getWriter();
    await writer.write(new Uint8Array(0));
    await writer.write('');

    await rejects(writer.write(new Uint8Array(1)), {
      message: 'The DigestStream was disposed.',
    });
    await rejects(stream.digest, { message: 'The DigestStream was disposed.' });
  },
};

export const disposeIsIdempotent = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();
    stream[Symbol.dispose]();
    stream[Symbol.dispose]();
    await rejects(stream.digest, { message: 'The DigestStream was disposed.' });
  },
};

// Closing resolves the digest; a later dispose must not overwrite it.
export const disposeAfterCloseIsNoOp = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    const digest = await stream.digest;
    stream[Symbol.dispose]();
    // Still resolved with the same value, not rejected.
    strictEqual(await stream.digest, digest);
  },
};

export const abortRejectsDigest = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const reason = new Error('boom');
    await writer.abort(reason);
    await rejects(stream.digest, { message: 'boom' });
  },
};

// Accessors are brand-checked rather than instanceof-checked, so they must
// reject a plain object even if its prototype is set correctly.
export const accessorsAreBrandChecked = {
  test() {
    const fake = Object.create(crypto.DigestStream.prototype);
    throws(() => fake.digest, { name: 'TypeError' });
    throws(() => fake.bytesWritten, { name: 'TypeError' });
    throws(() => fake[Symbol.dispose](), { name: 'TypeError' });

    const desc = Object.getOwnPropertyDescriptor(
      crypto.DigestStream.prototype,
      'digest'
    );
    strictEqual(typeof desc.get, 'function');
    strictEqual(desc.set, undefined);
  },
};

// The digest promise is marked handled, so abandoning it produces no
// unhandled-rejection report. This is a deliberate divergence from the C++
// implementation, which is why it is asserted here and not in the shared file.
export const abandonedDigestIsNotReported = {
  async test() {
    const reports = [];
    const onReport = (event) => {
      reports.push(event.reason);
      event.preventDefault();
    };
    addEventListener('unhandledrejection', onReport);
    try {
      {
        const stream = new crypto.DigestStream('md5');
        stream[Symbol.dispose]();
      }
      // Give the microtask queue and the rejection-report turn a chance to run.
      for (let i = 0; i < 10; i++) await Promise.resolve();
      await scheduler.wait(1);
      strictEqual(
        reports.length,
        0,
        `expected no unhandled rejection, got: ${reports.map(String)}`
      );

      // But marking handled must not propagate to derived promises: a consumer
      // that attaches a then() with no catch still gets a report.
      {
        const stream = new crypto.DigestStream('md5');
        stream.digest.then(() => {});
        stream[Symbol.dispose]();
      }
      for (let i = 0; i < 10; i++) await Promise.resolve();
      await scheduler.wait(1);
      strictEqual(reports.length, 1, 'derived promise should still report');
      strictEqual(reports[0].message, 'The DigestStream was disposed.');
    } finally {
      removeEventListener('unhandledrejection', onReport);
    }
  },
};

// Writes are not copied, so a view is read in place. Mutating the buffer after
// the write resolves must not change the digest.
export const chunkIsConsumedSynchronously = {
  async test() {
    const buf = new Uint8Array([1, 2, 3, 4]);
    const stream = new crypto.DigestStream('crc32');
    const writer = stream.getWriter();
    await writer.write(buf);
    buf.fill(0xff);
    await writer.close();
    const mutated = new Uint8Array(await stream.digest);

    const reference = new crypto.DigestStream('crc32');
    const refWriter = reference.getWriter();
    await refWriter.write(new Uint8Array([1, 2, 3, 4]));
    await refWriter.close();
    deepStrictEqual(mutated, new Uint8Array(await reference.digest));
  },
};

// Every algorithm the C++ implementation accepts must still be reachable, and
// each must produce a distinct digest length.
export const allAlgorithms = {
  async test() {
    const sizes = {
      md5: 16,
      'SHA-1': 20,
      'SHA-256': 32,
      'SHA-384': 48,
      'SHA-512': 64,
      crc32: 4,
      crc32c: 4,
      crc64nvme: 8,
    };
    for (const [name, size] of Object.entries(sizes)) {
      const stream = new crypto.DigestStream(name);
      const writer = stream.getWriter();
      await writer.write('abc');
      await writer.close();
      const digest = await stream.digest;
      strictEqual(digest.byteLength, size, `${name} digest size`);
    }
  },
};

// Algorithm names go to the same lookup as the C++ version, which is
// case-insensitive for OpenSSL digests but exact for the CRCs.
export const algorithmNameCasing = {
  async test() {
    // OpenSSL lookup tolerates case.
    for (const name of ['md5', 'MD5', 'sha-256', 'SHA-256']) {
      const stream = new crypto.DigestStream(name);
      stream[Symbol.dispose]();
    }
    // CRC names are matched exactly, so the uppercase forms fall through to the
    // OpenSSL lookup and fail.
    throws(() => new crypto.DigestStream('CRC32'));
    throws(() => new crypto.DigestStream('CRC32C'));
    throws(() => new crypto.DigestStream('CRC64NVME'));
  },
};

export const writeAfterCloseRejects = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.close();
    await rejects(writer.write(new Uint8Array(1)));
    // The digest is unaffected by the failed write.
    strictEqual((await stream.digest).byteLength, 16);
    strictEqual(stream.bytesWritten, 0n);
  },
};

export const multipleWritesAccumulate = {
  async test() {
    const check = new Uint8Array([
      198, 247, 195, 114, 100, 29, 210, 94, 15, 221, 240, 33, 83, 117, 86, 31,
    ]);
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const enc = new TextEncoder();
    // Mixed input kinds must concatenate in write order.
    await writer.write(enc.encode('hel'));
    await writer.write('lo');
    await writer.write(new Uint8Array(0));
    await writer.write(enc.encode('there'));
    await writer.close();
    deepStrictEqual(new Uint8Array(await stream.digest), check);
    strictEqual(stream.bytesWritten, 10n);
  },
};

// A DataView is an ArrayBufferView, so its byteOffset and byteLength must be
// honored rather than the whole backing buffer being hashed.
export const dataViewRespectsOffset = {
  async test() {
    const backing = new Uint8Array([9, 9, 1, 2, 3, 4, 9, 9]);
    const stream = new crypto.DigestStream('crc32');
    const writer = stream.getWriter();
    await writer.write(new DataView(backing.buffer, 2, 4));
    await writer.close();

    const reference = new crypto.DigestStream('crc32');
    const refWriter = reference.getWriter();
    await refWriter.write(new Uint8Array([1, 2, 3, 4]));
    await refWriter.close();

    deepStrictEqual(
      new Uint8Array(await stream.digest),
      new Uint8Array(await reference.digest)
    );
    strictEqual(stream.bytesWritten, 4n);
  },
};

// The digest is single-use: a second close must not try to finalize the native
// context again.
export const doubleCloseIsSafe = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.close();
    await rejects(writer.close());
    strictEqual((await stream.digest).byteLength, 16);
  },
};

export const constructorIsSubclassable = {
  async test() {
    class MyDigest extends crypto.DigestStream {
      constructor() {
        super('md5');
        this.tag = 'mine';
      }
    }
    const stream = new MyDigest();
    strictEqual(stream.tag, 'mine');
    ok(stream instanceof crypto.DigestStream);
    ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    strictEqual((await stream.digest).byteLength, 16);
  },
};

// Digests must be reachable from a plain `new WritableStream`-style teardown:
// releasing the writer without closing leaves the digest pending forever, which
// must not throw or crash.
export const abandonedWriterDoesNotThrow = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.write('hello');
    writer.releaseLock();
    strictEqual(stream.bytesWritten, 5n);
    strictEqual(stream.locked, false);
  },
};
