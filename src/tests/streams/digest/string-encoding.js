// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// String chunk encoding. By default string chunks are hashed as WTF-8,
// encoded per chunk exactly as V8 holds them (a lone surrogate is ED A0 80
// — NOT TextEncoder's U+FFFD substitution); this is the historical behavior
// existing digests depend on. toWellFormed: true opts into the
// substitution, with TextEncoderStream-style stateful pairing across chunk
// boundaries. Both implementations drive the same native encoder.

import { strictEqual, deepStrictEqual, notDeepStrictEqual } from 'node:assert';
import { digestOf } from 'digest-vectors';

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

export const stringChunksAreNotTextEncodedByDefault = {
  async test() {
    notDeepStrictEqual(
      await digestOf('md5', '\uD800'),
      await digestOf('md5', new TextEncoder().encode('\uD800')),
      'string chunks must not be TextEncoder-encoded by default'
    );
    // Well-formed strings agree with TextEncoder; only the lone-surrogate
    // case is special.
    deepStrictEqual(
      await digestOf('md5', 'h\u00e9llo\u{1F600}'),
      await digestOf('md5', new TextEncoder().encode('h\u00e9llo\u{1F600}'))
    );
  },
};

export const toWellFormedMatchesTextEncoder = {
  async test() {
    const opts = { toWellFormed: true };
    for (const input of [
      '\uD800',
      '\uDFFF',
      'a\uD800b',
      '\uD800\uD800',
      '\uDC00\uD800',
    ]) {
      deepStrictEqual(
        await digestOf('md5', input, opts),
        await digestOf('md5', new TextEncoder().encode(input)),
        `toWellFormed should match TextEncoder for ${JSON.stringify(input)}`
      );
    }
    // A valid surrogate pair is not a lone surrogate; the option changes
    // nothing for it.
    const pair = '\uD83D\uDE00';
    deepStrictEqual(
      await digestOf('md5', pair, opts),
      await digestOf('md5', pair)
    );
  },
};

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
    // Byte chunks are already bytes; the option cannot apply to them even
    // when they hold an invalid UTF-8 sequence.
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

export const toWellFormedDoesNotChangeBytesWritten = {
  async test() {
    // A lone surrogate is three bytes in WTF-8 and U+FFFD is three bytes
    // too, so bytesWritten is unaffected by the option.
    for (const options of [
      undefined,
      { toWellFormed: false },
      { toWellFormed: true },
    ]) {
      const { bytesWritten } = await digestOfChunks(
        'md5',
        ['a\uD800b'],
        options
      );
      strictEqual(bytesWritten, 5n);
    }
  },
};

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

// The field is coerced with ToBoolean rather than type-checked. Pinned for
// both implementations: the C++ side gets this from JSG and the TypeScript
// side matches it by hand.
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

export const toWellFormedJoinsSurrogatePairsAcrossChunks = {
  async test() {
    const opts = { toWellFormed: true };
    const enc = new TextEncoder();
    for (const [chunks, whole] of [
      [['ab\uD800', '\uDC00c'], 'ab\uD800\uDC00c'],
      [['\uD800', '\uDC00'], '\uD800\uDC00'],
      [['a', '\uD83D', '\uDE00', 'b'], 'a\uD83D\uDE00b'],
      [['\uD83D', '\uDE00\uD83D', '\uDE01'], '\uD83D\uDE00\uD83D\uDE01'],
      [['x\uD83D', '\uDE00yz'], 'x\uD83D\uDE00yz'],
    ]) {
      const split = await digestOfChunks('md5', chunks, opts);
      deepStrictEqual(
        split.digest,
        await digestOf('md5', whole, opts),
        `split ${JSON.stringify(chunks)} should match whole ${JSON.stringify(whole)}`
      );
      // The joined input is well-formed, so it must also match what any
      // other UTF-8 encoder produces.
      deepStrictEqual(split.digest, await digestOf('md5', enc.encode(whole)));
      strictEqual(
        split.bytesWritten,
        BigInt(enc.encode(whole).byteLength),
        `bytesWritten for ${JSON.stringify(chunks)}`
      );
    }
  },
};

export const toWellFormedFlushesDanglingLeadSurrogate = {
  async test() {
    // A lead surrogate held back from the final chunk is never paired, so
    // it flushes as U+FFFD when the stream closes.
    const opts = { toWellFormed: true };
    const enc = new TextEncoder();
    for (const [chunks, whole] of [
      [['ab\uD800'], 'ab\uD800'],
      [['ab', '\uD800'], 'ab\uD800'],
      [['a\uD800', 'b'], 'a\uD800b'],
      [['a\uD800', '\uD800b'], 'a\uD800\uD800b'],
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

export const defaultEncodingIsPerChunk = {
  async test() {
    // The default is deliberately NOT a streaming text encoder: a pair
    // split across writes becomes two separately-encoded lone surrogates.
    const split = await digestOfChunks('md5', ['ab\uD800', '\uDC00c']);
    const whole = await digestOf('md5', 'ab\uD800\uDC00c');
    notDeepStrictEqual(split.digest, whole);
    // Two lone surrogates at 3 bytes each rather than one 4-byte code point.
    strictEqual(split.bytesWritten, 3n + 3n + 3n);
  },
};
