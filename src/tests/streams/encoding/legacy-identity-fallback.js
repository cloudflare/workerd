// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without transformstream_enable_standard_constructor, the C++
// TransformStream constructor ignores the codec transformer and falls back
// to an IdentityTransformStream: TextEncoderStream still UTF-8-encodes
// strings (identity streams encode string writes) but with no surrogate
// pairing across chunks, its readable supports BYOB reads, and
// TextDecoderStream passes bytes through UNDECODED. Without
// capture_async_api_throws, an invalid chunk throws synchronously and the
// stream survives.

import { strictEqual, deepStrictEqual, ok, throws } from 'node:assert';

export const legacyEncoderIsIdentityPassThrough = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reader = tes.readable.getReader();
    const readPromise = reader.read();
    const writePromise = writer.write('hi');
    deepStrictEqual([...(await readPromise).value], [0x68, 0x69]);
    await writePromise;
    await writer.close();
  },
};

export const legacyEncoderNoSurrogatePairing = {
  async test() {
    // Each write is encoded in isolation: a pair split across writes
    // yields two replacement characters, never the astral character.
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reader = tes.readable.getReader();
    const chunks = [];
    const drained = (async () => {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push([...value]);
      }
    })();
    await Promise.all([writer.write('\uD83D'), writer.write('\uDE00')]).then(
      () => writer.close()
    );
    await drained;
    deepStrictEqual(chunks, [
      [0xef, 0xbf, 0xbd],
      [0xef, 0xbf, 0xbd],
    ]);
  },
};

export const legacyEncoderReadableSupportsByob = {
  test() {
    ok(new TextEncoderStream().readable.getReader({ mode: 'byob' }));
  },
};

export const legacyDecoderDoesNotDecode = {
  async test() {
    // Bytes pass through untouched; the reader sees Uint8Array, not string.
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const readPromise = reader.read();
    const writePromise = writer.write(Uint8Array.of(0xe4, 0xb8, 0xad));
    const { value } = await readPromise;
    ok(value instanceof Uint8Array);
    deepStrictEqual([...value], [0xe4, 0xb8, 0xad]);
    await writePromise;
    await writer.close();
    // The option getters still reflect the real decoder.
    strictEqual(tds.encoding, 'utf-8');
  },
};

export const legacyFatalDefaultsTrueWithOptionsBag = {
  test() {
    // Without pedantic_wpt, an options bag lacking `fatal` defaults it to
    // TRUE (the spec says false); with no bag at all the default is false.
    // The option mapping runs in the TextDecoderStream constructor
    // regardless of the identity-stream fallback.
    strictEqual(new TextDecoderStream().fatal, false);
    strictEqual(new TextDecoderStream('utf-8', {}).fatal, true);
    strictEqual(
      new TextDecoderStream('utf-8', { ignoreBOM: true }).fatal,
      true
    );
    strictEqual(new TextDecoderStream('utf-8', { fatal: false }).fatal, false);
  },
};

export const legacyDecoderInvalidChunkThrowsSynchronously = {
  async test() {
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    throws(
      () => writer.write(42),
      (err) => {
        strictEqual(err.constructor, TypeError);
        strictEqual(
          err.message,
          'This TransformStream is being used as a byte stream, but received ' +
            'an object of non-ArrayBuffer/ArrayBufferView type on its ' +
            'writable side.'
        );
        return true;
      }
    );
    // The stream survives: later traffic still flows.
    const readPromise = reader.read();
    const writePromise = writer.write(Uint8Array.of(0x6f, 0x6b));
    deepStrictEqual([...(await readPromise).value], [0x6f, 0x6b]);
    await writePromise;
    await writer.close();
  },
};
