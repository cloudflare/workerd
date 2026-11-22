// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, ok, rejects, strictEqual, throws } from 'node:assert';

const SEGMENT_SIZE = 1024;
const encoder = new TextEncoder();

function bytes(length, seed = 1) {
  const result = new Uint8Array(length);
  let x = seed;
  for (let i = 0; i < length; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    result[i] = x >>> 24;
  }
  return result;
}

function concat(chunks) {
  const total = chunks.reduce((n, c) => n + c.byteLength, 0);
  const result = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return result;
}

async function hkdfKey(seed = 7) {
  return crypto.subtle.importKey('raw', bytes(32, seed), 'HKDF', false, [
    'deriveKey',
    'deriveBits',
  ]);
}

async function optionsFor(algorithm, extra = {}) {
  const keys = {
    derive: 'HKDF',
    hash: 'SHA-256',
    key: await hkdfKey(),
    salt: encoder.encode('salt'),
    info: {
      prefix: encoder.encode('enc:object/'),
      index: 'decimal',
    },
  };
  if (algorithm === 'AES-CBC-HMAC-SHA256') {
    keys.macInfo = {
      prefix: encoder.encode('mac:object/'),
      index: 'decimal',
    };
  }
  return { algorithm, segmentSize: SEGMENT_SIZE, keys, ...extra };
}

// Writes `input` to the transform in chunks of `chunkSize` while reading its output.
async function transform(stream, input, chunkSize = 333) {
  const writer = stream.writable.getWriter();
  const writing = (async () => {
    for (let i = 0; i < input.byteLength; i += chunkSize) {
      await writer.write(input.slice(i, i + chunkSize));
    }
    await writer.close();
  })();
  const chunks = [];
  for await (const chunk of stream.readable) {
    ok(chunk instanceof Uint8Array);
    chunks.push(chunk);
  }
  await writing;
  return concat(chunks);
}

async function encrypt(algorithm, plaintext, extra = {}) {
  const stream = new crypto.SegmentEncryptionStream(
    await optionsFor(algorithm, extra)
  );
  return transform(stream, plaintext);
}

async function decrypt(algorithm, ciphertext, plaintextLength, extra = {}) {
  const stream = new crypto.SegmentDecryptionStream(
    await optionsFor(algorithm, { plaintextLength, ...extra })
  );
  return transform(stream, ciphertext);
}

const ALGORITHMS = ['AES-GCM', 'ChaCha20-Poly1305', 'AES-CBC-HMAC-SHA256'];
const LENGTHS = [
  0,
  1,
  SEGMENT_SIZE - 1,
  SEGMENT_SIZE,
  SEGMENT_SIZE + 1,
  3 * SEGMENT_SIZE + 17,
];

export const roundTrip = {
  async test() {
    for (const algorithm of ALGORITHMS) {
      for (const length of LENGTHS) {
        const plaintext = bytes(length, length + 3);
        const ciphertext = await encrypt(algorithm, plaintext);
        strictEqual(
          ciphertext.byteLength,
          crypto.SegmentEncryptionStream.encryptedLength(length, {
            algorithm,
            segmentSize: SEGMENT_SIZE,
          }),
          `${algorithm} ${length}`
        );
        deepStrictEqual(
          await decrypt(algorithm, ciphertext, length),
          plaintext
        );
      }
    }
  },
};

export const encryptedLength = {
  test() {
    const length = (algorithm, n) =>
      crypto.SegmentEncryptionStream.encryptedLength(n, {
        algorithm,
        segmentSize: 65536,
      });
    strictEqual(length('AES-GCM', 0), 0);
    strictEqual(length('AES-GCM', 65536), 65564);
    strictEqual(length('AES-GCM', 65537), 65564 + 29);
    strictEqual(length('AES-CBC-HMAC-SHA256', 65536), 65600);
    strictEqual(length('AES-CBC-HMAC-SHA256', 15), 16 + 16 + 32);
    strictEqual(length('AES-CBC-HMAC-SHA256', 16), 16 + 32 + 32);
    throws(() => length('AES-GCM', -1), RangeError);
    throws(
      () =>
        crypto.SegmentEncryptionStream.encryptedLength(1, {
          algorithm: 'AES-GCM',
          segmentSize: 100,
        }),
      RangeError
    );
    throws(
      () =>
        crypto.SegmentEncryptionStream.encryptedLength(1, {
          algorithm: 'AES-CTR',
          segmentSize: 65536,
        }),
      { name: 'NotSupportedError' }
    );
  },
};

// Splits a ciphertext into sealed segments.
function* segments(ciphertext, sealedSize) {
  for (let i = 0; i < ciphertext.byteLength; i += sealedSize) {
    yield ciphertext.subarray(i, i + sealedSize);
  }
}

async function segmentKey(index, prefix, algorithm, usages) {
  return crypto.subtle.deriveKey(
    {
      name: 'HKDF',
      hash: 'SHA-256',
      salt: encoder.encode('salt'),
      info: encoder.encode(`${prefix}${index}`),
    },
    await hkdfKey(),
    algorithm,
    false,
    usages
  );
}

// Each segment is a WebCrypto AES-GCM (or AES-CBC + HMAC) message under its own HKDF key.
export const webCryptoInterop = {
  async test() {
    const plaintext = bytes(2 * SEGMENT_SIZE + 100, 42);
    const firstSegment = 5;

    const gcm = await encrypt('AES-GCM', plaintext, { firstSegment });
    const gcmPlain = [];
    let index = firstSegment;
    for (const segment of segments(gcm, SEGMENT_SIZE + 28)) {
      const key = await segmentKey(
        index++,
        'enc:object/',
        { name: 'AES-GCM', length: 256 },
        ['decrypt']
      );
      gcmPlain.push(
        new Uint8Array(
          await crypto.subtle.decrypt(
            { name: 'AES-GCM', iv: segment.subarray(0, 12) },
            key,
            segment.subarray(12)
          )
        )
      );
    }
    deepStrictEqual(concat(gcmPlain), plaintext);

    const cbc = await encrypt('AES-CBC-HMAC-SHA256', plaintext, {
      firstSegment,
    });
    const cbcPlain = [];
    index = firstSegment;
    for (const segment of segments(cbc, SEGMENT_SIZE + 16 + 16 + 32)) {
      const signed = segment.subarray(0, segment.byteLength - 32);
      const macKey = await segmentKey(
        index,
        'mac:object/',
        { name: 'HMAC', hash: 'SHA-256', length: 256 },
        ['verify']
      );
      ok(
        await crypto.subtle.verify(
          'HMAC',
          macKey,
          segment.subarray(segment.byteLength - 32),
          signed
        )
      );
      const key = await segmentKey(
        index++,
        'enc:object/',
        { name: 'AES-CBC', length: 256 },
        ['decrypt']
      );
      cbcPlain.push(
        new Uint8Array(
          await crypto.subtle.decrypt(
            { name: 'AES-CBC', iv: signed.subarray(0, 16) },
            key,
            signed.subarray(16)
          )
        )
      );
    }
    deepStrictEqual(concat(cbcPlain), plaintext);

    // And the other way around: segments sealed with WebCrypto open in the stream.
    const sealed = [];
    index = firstSegment;
    for (let i = 0; i < plaintext.byteLength; i += SEGMENT_SIZE) {
      const key = await segmentKey(
        index++,
        'enc:object/',
        { name: 'AES-GCM', length: 256 },
        ['encrypt']
      );
      const iv = crypto.getRandomValues(new Uint8Array(12));
      const ct = await crypto.subtle.encrypt(
        { name: 'AES-GCM', iv },
        key,
        plaintext.subarray(i, i + SEGMENT_SIZE)
      );
      sealed.push(iv, new Uint8Array(ct));
    }
    deepStrictEqual(
      await decrypt('AES-GCM', concat(sealed), plaintext.byteLength, {
        firstSegment,
      }),
      plaintext
    );
  },
};

export const indexEncodings = {
  async test() {
    const plaintext = bytes(SEGMENT_SIZE + 1, 9);
    for (const index of ['uint32be', 'uint64be']) {
      const options = await optionsFor('AES-GCM', { firstSegment: 3 });
      options.keys.info = {
        prefix: encoder.encode('p'),
        index,
        suffix: encoder.encode('s'),
      };
      const ciphertext = await transform(
        new crypto.SegmentEncryptionStream(options),
        plaintext
      );
      // The first segment's key is HKDF(info = 'p' || encode(3) || 's').
      const encoded = new Uint8Array(index === 'uint32be' ? 4 : 8);
      encoded[encoded.length - 1] = 3;
      const key = await crypto.subtle.deriveKey(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: encoder.encode('salt'),
          info: concat([encoder.encode('p'), encoded, encoder.encode('s')]),
        },
        await hkdfKey(),
        { name: 'AES-GCM', length: 256 },
        false,
        ['decrypt']
      );
      const first = ciphertext.subarray(0, SEGMENT_SIZE + 28);
      deepStrictEqual(
        new Uint8Array(
          await crypto.subtle.decrypt(
            { name: 'AES-GCM', iv: first.subarray(0, 12) },
            key,
            first.subarray(12)
          )
        ),
        plaintext.subarray(0, SEGMENT_SIZE)
      );
    }

    const options = await optionsFor('AES-GCM', { firstSegment: 2 ** 32 });
    options.keys.info.index = 'uint32be';
    throws(() => new crypto.SegmentEncryptionStream(options), RangeError);
  },
};

// A read from segment k onward, trimmed to a range within it.
export const ranges = {
  async test() {
    const plaintext = bytes(5 * SEGMENT_SIZE + 300, 11);
    for (const algorithm of ALGORITHMS) {
      const ciphertext = await encrypt(algorithm, plaintext);
      const sealedSize = crypto.SegmentEncryptionStream.encryptedLength(
        SEGMENT_SIZE,
        {
          algorithm,
          segmentSize: SEGMENT_SIZE,
        }
      );
      for (const [start, length] of [
        [0, 1],
        [10, 100],
        [SEGMENT_SIZE - 1, 2],
        [2 * SEGMENT_SIZE + 5, 2 * SEGMENT_SIZE],
        [5 * SEGMENT_SIZE + 299, 1],
        [3 * SEGMENT_SIZE, 10 * SEGMENT_SIZE],
      ]) {
        const firstSegment = Math.floor(start / SEGMENT_SIZE);
        const end = Math.min(start + length, plaintext.byteLength);
        const lastSegment = Math.ceil(end / SEGMENT_SIZE);
        const plaintextLength =
          Math.min(lastSegment * SEGMENT_SIZE, plaintext.byteLength) -
          firstSegment * SEGMENT_SIZE;
        const input = ciphertext.subarray(
          firstSegment * sealedSize,
          firstSegment * sealedSize +
            crypto.SegmentEncryptionStream.encryptedLength(plaintextLength, {
              algorithm,
              segmentSize: SEGMENT_SIZE,
            })
        );
        const output = await decrypt(algorithm, input, plaintextLength, {
          firstSegment,
          outputOffset: start - firstSegment * SEGMENT_SIZE,
          outputLength: length,
        });
        deepStrictEqual(
          output,
          plaintext.subarray(start, end),
          `${algorithm} ${start}+${length}`
        );
      }
    }
  },
};

async function rejectsOperation(promise, message) {
  await rejects(promise, (e) => {
    strictEqual(e.name, 'OperationError');
    ok(message.test(e.message), e.message);
    return true;
  });
}

export const tampering = {
  async test() {
    const length = 3 * SEGMENT_SIZE + 10;
    const plaintext = bytes(length, 5);
    for (const algorithm of ALGORITHMS) {
      const ciphertext = await encrypt(algorithm, plaintext);
      const sealedSize = crypto.SegmentEncryptionStream.encryptedLength(
        SEGMENT_SIZE,
        {
          algorithm,
          segmentSize: SEGMENT_SIZE,
        }
      );

      // A flipped bit anywhere in a segment.
      for (const offset of [
        0,
        20,
        sealedSize - 1,
        sealedSize + 100,
        ciphertext.length - 1,
      ]) {
        const corrupt = ciphertext.slice();
        corrupt[offset] ^= 1;
        const segment = Math.floor(offset / sealedSize);
        await rejectsOperation(
          decrypt(algorithm, corrupt, length),
          new RegExp(`Segment ${segment} failed authentication`)
        );
      }

      // Swapped segments.
      const swapped = concat([
        ciphertext.subarray(sealedSize, 2 * sealedSize),
        ciphertext.subarray(0, sealedSize),
        ciphertext.subarray(2 * sealedSize),
      ]);
      await rejectsOperation(
        decrypt(algorithm, swapped, length),
        /Segment 0 failed/
      );

      // The same segments at a different index.
      await rejectsOperation(
        decrypt(algorithm, ciphertext, length, { firstSegment: 1 }),
        /Segment 1 failed/
      );

      // Truncated at a segment boundary, and inside a segment.
      await rejectsOperation(
        decrypt(algorithm, ciphertext.subarray(0, 3 * sealedSize), length),
        /input ended after/
      );
      await rejectsOperation(
        decrypt(
          algorithm,
          ciphertext.subarray(0, ciphertext.length - 1),
          length
        ),
        /input ended after/
      );

      // Trailing data.
      await rejectsOperation(
        decrypt(algorithm, concat([ciphertext, new Uint8Array(1)]), length),
        /continues past the end/
      );
    }
  },
};

// Segments authenticated before the failure are still delivered, then the stream errors.
export const outputBeforeFailure = {
  async test() {
    const plaintext = bytes(3 * SEGMENT_SIZE, 8);
    const ciphertext = await encrypt('AES-GCM', plaintext);
    const corrupt = ciphertext.slice();
    corrupt[corrupt.length - 1] ^= 1;
    const stream = new crypto.SegmentDecryptionStream(
      await optionsFor('AES-GCM', { plaintextLength: plaintext.byteLength })
    );
    const writer = stream.writable.getWriter();
    const writing = writer.write(corrupt).then(
      () => writer.close(),
      (e) => e
    );
    const reader = stream.readable.getReader();
    const received = [];
    let error;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        received.push(value);
      }
    } catch (e) {
      error = e;
    }
    strictEqual(error?.name, 'OperationError');
    deepStrictEqual(concat(received), plaintext.subarray(0, 2 * SEGMENT_SIZE));
    strictEqual((await writing)?.name, 'OperationError');
  },
};

export const plaintextLengthOnEncrypt = {
  async test() {
    const plaintext = bytes(100);
    deepStrictEqual(
      await decrypt(
        'AES-GCM',
        await encrypt('AES-GCM', plaintext, { plaintextLength: 100 }),
        100
      ),
      plaintext
    );
    await rejectsOperation(
      encrypt('AES-GCM', plaintext, { plaintextLength: 99 }),
      /more than the plaintextLength/
    );
    await rejectsOperation(
      encrypt('AES-GCM', plaintext, { plaintextLength: 101 }),
      /but the plaintextLength is 101/
    );
  },
};

export const invalidOptions = {
  async test() {
    const base = await optionsFor('AES-GCM');
    throws(() => new crypto.SegmentDecryptionStream(base), TypeError);
    throws(
      () => new crypto.SegmentEncryptionStream({ ...base, outputOffset: 1 }),
      TypeError
    );
    throws(
      () =>
        new crypto.SegmentDecryptionStream({
          ...base,
          plaintextLength: 10,
          outputOffset: 11,
        }),
      RangeError
    );
    throws(
      () => new crypto.SegmentEncryptionStream({ ...base, segmentSize: 1.5 }),
      RangeError
    );
    throws(
      () =>
        new crypto.SegmentEncryptionStream({ ...base, segmentSize: 2 ** 25 }),
      RangeError
    );
    throws(() => new crypto.SegmentEncryptionStream({ ...base, length: 192 }), {
      name: 'OperationError',
    });
    throws(
      () =>
        new crypto.SegmentEncryptionStream({
          ...base,
          keys: { ...base.keys, derive: 'PBKDF2' },
        }),
      { name: 'NotSupportedError' }
    );
    throws(
      () =>
        new crypto.SegmentEncryptionStream({
          ...base,
          keys: { ...base.keys, info: { ...base.keys.info, index: 'hex' } },
        }),
      TypeError
    );
    // CBC needs a MAC key, and the AEADs refuse one.
    throws(
      () =>
        new crypto.SegmentEncryptionStream({
          ...base,
          algorithm: 'AES-CBC-HMAC-SHA256',
        }),
      TypeError
    );
    throws(
      () =>
        new crypto.SegmentEncryptionStream({
          ...base,
          keys: { ...base.keys, macInfo: base.keys.info },
        }),
      TypeError
    );

    // The key must be an HKDF key that may derive.
    const aesKey = await crypto.subtle.generateKey(
      { name: 'AES-GCM', length: 256 },
      false,
      ['encrypt']
    );
    throws(
      () =>
        new crypto.SegmentEncryptionStream({
          ...base,
          keys: { ...base.keys, key: aesKey },
        }),
      { name: 'InvalidAccessError' }
    );
  },
};

// Piping between native streams, as a Worker proxying a body would.
export const nativePipe = {
  async test() {
    const plaintext = bytes(10 * SEGMENT_SIZE + 7, 13);
    const encryptor = new crypto.SegmentEncryptionStream(
      await optionsFor('AES-GCM')
    );
    const ciphertext = new Uint8Array(
      await new Response(
        new Response(plaintext).body.pipeThrough(encryptor)
      ).arrayBuffer()
    );
    const decryptor = new crypto.SegmentDecryptionStream(
      await optionsFor('AES-GCM', { plaintextLength: plaintext.byteLength })
    );
    const output = new Uint8Array(
      await new Response(
        new Response(ciphertext).body.pipeThrough(decryptor)
      ).arrayBuffer()
    );
    deepStrictEqual(output, plaintext);
  },
};

// Like a TransformStream, a write waits once the readable side has a backlog.
export const backpressure = {
  async test() {
    const stream = new crypto.SegmentEncryptionStream(
      await optionsFor('AES-GCM')
    );
    const writer = stream.writable.getWriter();
    await writer.write(bytes(4 * SEGMENT_SIZE));
    let secondDone = false;
    const second = writer.write(bytes(SEGMENT_SIZE)).then(() => {
      secondDone = true;
    });
    await scheduler.wait(10);
    strictEqual(secondDone, false);

    // Reading the backlog lets the second write through.
    const reader = stream.readable.getReader();
    const expected = 5 * (SEGMENT_SIZE + 28);
    let read = 0;
    while (read < expected) {
      const { value } = await reader.read();
      read += value.byteLength;
    }
    await second;
    strictEqual(read, expected);
  },
};

// Canceling the readable side fails later writes.
export const cancellation = {
  async test() {
    const stream = new crypto.SegmentEncryptionStream(
      await optionsFor('AES-GCM')
    );
    const writer = stream.writable.getWriter();
    const reason = new Error('stop');
    await stream.readable.cancel(reason);
    await rejects(
      writer.write(bytes(10)),
      (e) => e === reason || e.message === 'stop'
    );
  },
};
