// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  strictEqual,
  deepStrictEqual,
  notDeepStrictEqual,
  rejects,
  throws,
} from 'node:assert';
import { Buffer } from 'node:buffer';

async function digestOf(algorithm, chunk, options) {
  const stream = new crypto.DigestStream(algorithm, options);
  const writer = stream.getWriter();
  await writer.write(chunk);
  await writer.close();
  return new Uint8Array(await stream.digest);
}

// A lone surrogate has no UTF-8 encoding, so string chunks have two possible
// byte sequences. By default the runtime emits WTF-8 (ED A0 80); TextEncoder
// substitutes U+FFFD (EF BF BD). The default must stay WTF-8 for backwards
// compatibility, which is what this pins — for BOTH implementations, so it
// fails loudly if either drifts.
export const stringChunksAreNotTextEncodedByDefault = {
  async test() {
    const viaString = await digestOf('md5', '\uD800');
    const viaEncoder = await digestOf(
      'md5',
      new TextEncoder().encode('\uD800')
    );
    notDeepStrictEqual(
      viaString,
      viaEncoder,
      'string chunks must not be TextEncoder-encoded by default'
    );

    // Well-formed strings agree with TextEncoder, so only the lone-surrogate
    // case is special.
    deepStrictEqual(
      await digestOf('md5', 'h\u00e9llo\u{1F600}'),
      await digestOf('md5', new TextEncoder().encode('h\u00e9llo\u{1F600}'))
    );
  },
};

// toWellFormed: true opts into the substitution, making string chunks agree
// with TextEncoder — and therefore with every other UTF-8 producer.
export const toWellFormedMatchesTextEncoder = {
  async test() {
    const opts = { toWellFormed: true };
    for (const input of [
      '\uD800', // lone high surrogate
      '\uDFFF', // lone low surrogate
      'a\uD800b', // surrogate between well-formed text
      '\uD800\uD800', // two lone high surrogates
      '\uDC00\uD800', // reversed pair: both are lone
    ]) {
      deepStrictEqual(
        await digestOf('md5', input, opts),
        await digestOf('md5', new TextEncoder().encode(input)),
        `toWellFormed should match TextEncoder for ${JSON.stringify(input)}`
      );
    }

    // A valid surrogate pair is not a lone surrogate, so the option changes
    // nothing for it.
    const pair = '\uD83D\uDE00';
    deepStrictEqual(
      await digestOf('md5', pair, opts),
      await digestOf('md5', pair)
    );
  },
};

// The option only affects strings, and only strings containing lone
// surrogates. Everything else must hash identically either way, or the option
// would be a silent breaking change for existing callers who set it.
export const toWellFormedIsInertForValidInput = {
  async test() {
    const enc = new TextEncoder();
    for (const input of ['', 'hello', 'h\u00e9llo\u{1F600}\u{10FFFF}']) {
      deepStrictEqual(
        await digestOf('md5', input, { toWellFormed: true }),
        await digestOf('md5', input, { toWellFormed: false }),
        `well-formed input must be unaffected: ${JSON.stringify(input)}`
      );
    }
    // Byte chunks are already bytes; the option cannot apply to them even when
    // they hold an invalid UTF-8 sequence.
    for (const bytes of [
      enc.encode('hello'),
      new Uint8Array([0xed, 0xa0, 0x80]), // WTF-8 lone surrogate
      new Uint8Array([0xff, 0xfe]), // not UTF-8 at all
    ]) {
      deepStrictEqual(
        await digestOf('md5', bytes, { toWellFormed: true }),
        await digestOf('md5', bytes, { toWellFormed: false })
      );
    }
  },
};

// The two encodings agree on byte count: a lone surrogate is three bytes in
// WTF-8, and U+FFFD is three bytes too. So bytesWritten is unaffected.
export const toWellFormedDoesNotChangeBytesWritten = {
  async test() {
    for (const options of [
      undefined,
      { toWellFormed: false },
      { toWellFormed: true },
    ]) {
      const stream = new crypto.DigestStream('md5', options);
      const writer = stream.getWriter();
      await writer.write('a\uD800b');
      await writer.close();
      await stream.digest;
      strictEqual(stream.bytesWritten, 5n);
    }
  },
};

// Every spelling of "not requested" must give the historical WTF-8 behavior,
// since anything else would break existing callers.
export const toWellFormedDefaultsToFalse = {
  async test() {
    const expected = await digestOf('md5', '\uD800');
    for (const options of [
      undefined,
      {},
      { toWellFormed: undefined },
      { toWellFormed: false },
      { toWellFormed: 0 },
      { toWellFormed: '' },
      { toWellFormed: null },
      { somethingElse: true },
    ]) {
      deepStrictEqual(
        await digestOf('md5', '\uD800', options),
        expected,
        `should default to WTF-8: ${JSON.stringify(options)}`
      );
    }
  },
};

async function digestOfChunks(algorithm, chunks, options) {
  const stream = new crypto.DigestStream(algorithm, options);
  const writer = stream.getWriter();
  for (const chunk of chunks) {
    await writer.write(chunk);
  }
  await writer.close();
  return {
    digest: new Uint8Array(await stream.digest),
    bytesWritten: stream.bytesWritten,
  };
}

// toWellFormed asks for the input to be treated as text, so a surrogate pair
// split across two writes is one code point that happens to arrive in two
// pieces — not two unpaired surrogates. The encoder therefore holds a trailing
// lead surrogate back until the next chunk, exactly as TextEncoderStream does.
export const toWellFormedJoinsSurrogatePairsAcrossChunks = {
  async test() {
    const opts = { toWellFormed: true };
    const enc = new TextEncoder();

    for (const [chunks, whole] of [
      [['ab\uD800', '\uDC00c'], 'ab\uD800\uDC00c'],
      // The pair is the entire input, split at the boundary.
      [['\uD800', '\uDC00'], '\uD800\uDC00'],
      // Lead ends a chunk that is otherwise empty of text.
      [['a', '\uD83D', '\uDE00', 'b'], 'a\uD83D\uDE00b'],
      // Several pairs, each split.
      [['\uD83D', '\uDE00\uD83D', '\uDE01'], '\uD83D\uDE00\uD83D\uDE01'],
      // A pair split with the second half followed by more text.
      [['x\uD83D', '\uDE00yz'], 'x\uD83D\uDE00yz'],
    ]) {
      const split = await digestOfChunks('md5', chunks, opts);
      deepStrictEqual(
        split.digest,
        await digestOf('md5', whole, opts),
        `split ${JSON.stringify(chunks)} should match whole ${JSON.stringify(whole)}`
      );
      // And since the joined input is well-formed, it must also match what any
      // other UTF-8 encoder would produce.
      deepStrictEqual(split.digest, await digestOf('md5', enc.encode(whole)));
      strictEqual(
        split.bytesWritten,
        BigInt(enc.encode(whole).byteLength),
        `bytesWritten for ${JSON.stringify(chunks)}`
      );
    }
  },
};

// A lead surrogate held back from the final chunk is never paired, so it is an
// unpaired surrogate and must be flushed as U+FFFD when the stream closes.
export const toWellFormedFlushesDanglingLeadSurrogate = {
  async test() {
    const opts = { toWellFormed: true };
    const enc = new TextEncoder();

    for (const [chunks, whole] of [
      [['ab\uD800'], 'ab\uD800'],
      [['ab', '\uD800'], 'ab\uD800'],
      // Lead followed by a chunk that cannot pair with it: the lead resolves to
      // U+FFFD and the next chunk is encoded normally.
      [['a\uD800', 'b'], 'a\uD800b'],
      [['a\uD800', '\uD800b'], 'a\uD800\uD800b'],
      // A trail surrogate with nothing pending is unpaired on its own.
      [['a', '\uDC00b'], 'a\uDC00b'],
      [['\uDC00\uD800'], '\uDC00\uD800'],
    ]) {
      const split = await digestOfChunks('md5', chunks, opts);
      deepStrictEqual(
        split.digest,
        await digestOf('md5', enc.encode(whole)),
        `${JSON.stringify(chunks)} should encode as ${JSON.stringify(whole)}`
      );
      strictEqual(split.bytesWritten, BigInt(enc.encode(whole).byteLength));
    }
  },
};

// The default encoding is deliberately NOT a streaming text encoder: each chunk
// is encoded exactly as V8 holds it, so a pair split across writes becomes two
// separately-encoded lone surrogates rather than one code point. This differs
// from the whole-string digest, and that difference is the historical behavior —
// changing it would silently alter existing callers' digests.
export const defaultEncodingIsPerChunk = {
  async test() {
    const split = await digestOfChunks('md5', ['ab\uD800', '\uDC00c']);
    const whole = await digestOf('md5', 'ab\uD800\uDC00c');
    notDeepStrictEqual(split.digest, whole);
    // Two lone surrogates at 3 bytes each, rather than one 4-byte code point.
    strictEqual(split.bytesWritten, 3n + 3n + 3n);
  },
};

// Algorithm names are matched case-insensitively, for the CRCs as well as for
// the OpenSSL digests. Every casing of a name must select the same algorithm,
// so the digests are compared rather than merely checked for not throwing.
export const algorithmNamesAreCaseInsensitive = {
  async test() {
    const variants = {
      crc32: ['crc32', 'CRC32', 'Crc32', 'cRc32'],
      crc32c: ['crc32c', 'CRC32C', 'Crc32c', 'crc32C', 'cRc32C'],
      crc64nvme: ['crc64nvme', 'CRC64NVME', 'Crc64Nvme', 'crc64NVME'],
      md5: ['md5', 'MD5', 'Md5'],
      'sha-256': ['sha-256', 'SHA-256', 'Sha-256'],
    };

    for (const [canonical, names] of Object.entries(variants)) {
      const expected = await digestOf(canonical, 'hello');
      for (const name of names) {
        deepStrictEqual(
          await digestOf(name, 'hello'),
          expected,
          `${name} should select the same algorithm as ${canonical}`
        );
      }
    }
  },
};

// Case-insensitivity must not turn a nonsense name into a match: the comparison
// is over the whole name, so a CRC name with anything extra still fails.
export const crcNamesStillRequireAnExactSpelling = {
  test() {
    for (const name of [
      'crc',
      'crc3',
      'crc322',
      'crc32d',
      ' crc32',
      'crc32 ',
      'crc-32',
      'crc64',
      'crc64nvm',
      'crc64nvmex',
      'nvme',
    ]) {
      throws(
        () => new crypto.DigestStream(name),
        { name: 'NotSupportedError' },
        `${JSON.stringify(name)} should not be a valid algorithm`
      );
    }
  },
};

// The option bag follows Web IDL dictionary rules: undefined and null are an
// empty bag, any object is read for its fields, and a primitive is a TypeError.
// Arrays and functions count as objects, so they are accepted and simply carry
// no field.
export const optionBagAcceptsObjectsAndRejectsPrimitives = {
  test() {
    for (const options of [undefined, null, {}, [], () => {}, new Date()]) {
      const stream = new crypto.DigestStream('md5', options);
      stream[Symbol.dispose]();
    }

    for (const options of [0, 1, '', 'x', true, false, 1n, Symbol.iterator]) {
      throws(
        () => new crypto.DigestStream('md5', options),
        {
          name: 'TypeError',
          message:
            "Failed to construct 'DigestStream': constructor parameter 2 is " +
            "not of type 'Options'.",
        },
        `should reject primitive option bag: ${String(options)}`
      );
    }
  },
};

// Argument type-checking happens before the algorithm name is looked up, so a
// bad option bag is reported ahead of an unrecognized algorithm. 'foo' is a
// perfectly good string — it only fails later, when the digest is created.
export const argumentTypesAreCheckedBeforeAlgorithmLookup = {
  test() {
    throws(() => new crypto.DigestStream('foo', 0), {
      name: 'TypeError',
      message:
        "Failed to construct 'DigestStream': constructor parameter 2 is not " +
        "of type 'Options'.",
    });
    // With a well-typed option bag, the algorithm lookup is reached and fails.
    throws(() => new crypto.DigestStream('foo', { toWellFormed: true }), {
      name: 'NotSupportedError',
    });
  },
};

// The field is coerced with ToBoolean rather than type-checked, so any truthy
// value opts in. Pinned for both implementations because the C++ side gets
// this from JSG and the TypeScript side has to match it by hand.
export const toWellFormedIsCoerced = {
  async test() {
    const expected = await digestOf('md5', '\uD800', { toWellFormed: true });
    for (const value of [true, 1, 'false', [], {}, Symbol.iterator]) {
      deepStrictEqual(
        await digestOf('md5', '\uD800', { toWellFormed: value }),
        expected,
        `truthy ${String(value)} should opt in`
      );
    }
  },
};

export const digeststream = {
  async test() {
    {
      const check = new Uint8Array([
        198, 247, 195, 114, 100, 29, 210, 94, 15, 221, 240, 33, 83, 117, 86, 31,
      ]);

      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      const enc = new TextEncoder();

      writer.write(enc.encode('hello'));
      writer.write(enc.encode('there'));
      writer.close();

      const digest = new Uint8Array(await stream.digest);

      strictEqual(stream.bytesWritten, 10n);
      deepStrictEqual(digest, check);
    }

    {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      const enc = new TextEncoder();

      writer.write(enc.encode('hello'));
      writer.write(enc.encode('there'));
      writer.abort(new Error('boom'));

      await rejects(stream.digest);
    }

    {
      // Creating for other known types works...
      new crypto.DigestStream('SHA-256');
      new crypto.DigestStream('SHA-384');
      new crypto.DigestStream('SHA-512');
      new crypto.DigestStream('crc32');

      // But fails for unknown digest names...
      throws(() => new crypto.DigestStream('foo'));
    }

    (async () => {
      let digestPromise;
      {
        digestPromise = new crypto.DigestStream('md5').digest;
      }
      globalThis.gc();
      await digestPromise;
      throw new Error('The promise should not have resolved');
    })();

    {
      const enc = new TextEncoder();
      const check = new Uint8Array([
        93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
      ]);
      const digestStream = new crypto.DigestStream('md5');
      const writer = digestStream.getWriter();
      await writer.write(enc.encode('hello'));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const check = new Uint8Array([
        93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
      ]);
      const digestStream = new crypto.DigestStream('md5');
      const writer = digestStream.getWriter();
      await writer.write('hello');
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const check = new Uint8Array([
        70, 54, 153, 61, 62, 29, 164, 233, 214, 184, 248, 123, 121, 232, 247,
        198, 208, 24, 88, 13, 82, 102, 25, 80, 234, 188, 56, 69, 197, 137, 122,
        77,
      ]);
      const digestStream = new crypto.DigestStream('SHA-256');
      const writer = digestStream.getWriter();
      await writer.write(new Uint32Array([1, 2, 3]));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const check = new Uint8Array([
        70, 54, 153, 61, 62, 29, 164, 233, 214, 184, 248, 123, 121, 232, 247,
        198, 208, 24, 88, 13, 82, 102, 25, 80, 234, 188, 56, 69, 197, 137, 122,
        77,
      ]);
      const digestStream = new crypto.DigestStream('SHA-256');
      const writer = digestStream.getWriter();
      // Ensures that byteOffset is correctly handled.
      await writer.write(new Uint32Array([0, 1, 2, 3]).subarray(1));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const check = new Uint8Array([176, 224, 34, 147]);
      const digestStream = new crypto.DigestStream('crc32');
      const writer = digestStream.getWriter();
      await writer.write(new Uint32Array([1, 2, 3]));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const digestStream = new crypto.DigestStream('md5');
      const writer = digestStream.getWriter();

      try {
        await writer.write(123);
        throw new Error('should have failed');
      } catch (err) {
        strictEqual(
          err.message,
          'DigestStream is a byte stream but received an object ' +
            'of non-ArrayBuffer/ArrayBufferView/string type on its writable side.'
        );
      }
    }

    // AWS CRC tests, source:
    // https://github.com/aws/aws-sdk-js-v3/blob/c3f3d0a1c652c88fef5859881c9a12cfc8df61c1/packages/middleware-flexible-checksums/src/middleware-flexible-checksums.integ.spec.ts#L21
    const testCases = [
      ['', 'crc32', 'AAAAAA=='],
      ['abc', 'crc32', 'NSRBwg=='],
      ['Hello world', 'crc32', 'i9aeUg=='],

      ['', 'crc32c', 'AAAAAA=='],
      ['abc', 'crc32c', 'Nks/tw=='],
      ['Hello world', 'crc32c', 'crUfeA=='],

      ['', 'crc64nvme', 'AAAAAAAAAAA='],
      ['abc', 'crc64nvme', 'BeXKuz/B+us='],
      ['Hello world', 'crc64nvme', 'OOJZ0D8xKts='],

      ['', 'SHA-1', '2jmj7l5rSw0yVb/vlWAYkK/YBwk='],
      ['abc', 'SHA-1', 'qZk+NkcGgWq6PiVxeFDCbJzQ2J0='],
      ['Hello world', 'SHA-1', 'e1AsOh9IyGCa4hLN+2Od7jlnP14='],

      ['', 'SHA-256', '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='],
      ['abc', 'SHA-256', 'ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0='],
      [
        'Hello world',
        'SHA-256',
        'ZOyIygCyaOW6GjVnihtTFtIS9PNmskdyMlNKiuyjfzw=',
      ],
    ];
    {
      for (const [body, algorithm, expected] of testCases) {
        const digestStream = new crypto.DigestStream(algorithm);
        const writer = digestStream.getWriter();
        const enc = new TextEncoder();
        writer.write(enc.encode(body));
        writer.close();
        const digest = await digestStream.digest;
        deepStrictEqual(digest, Buffer.from(expected, 'base64').buffer);
      }
    }

    // Creating and not using a digest stream doesn't crash
    new crypto.DigestStream('SHA-1');
  },
};

export const digestStreamNoEnd = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const enc = new TextEncoder();

    writer.write(enc.encode('hello'));
    writer.write(enc.encode('there'));
    // stream never ends, should not crash.
  },
};

export const digestStreamDisposable = {
  async test() {
    const enc = new TextEncoder();
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();

    const writer = stream.getWriter();

    try {
      await writer.write(enc.encode('hello'));
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The DigestStream was disposed.');
    }

    // Calling dispose again should have no impact
    stream[Symbol.dispose]();
  },
};

export const digestStreamLargeChunks = {
  async test() {
    {
      const check = new Uint8Array([0x13, 0xb3, 0xf0, 0x58]);
      const digestStream = new crypto.DigestStream('crc32');
      const writer = digestStream.getWriter();
      await writer.write(Buffer.alloc(1024, 'a'));
      await writer.write(Buffer.alloc(1024, 'b'));
      await writer.write(Buffer.alloc(1024, 'c'));
      await writer.write(Buffer.alloc(1024 * 1024, 'd'));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }
  },
};
