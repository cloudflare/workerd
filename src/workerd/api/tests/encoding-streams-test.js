// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Test TextEncoderStream/TextDecoderStream pipeline
export const textEncoderDecoderPipeline = {
  async test() {
    // encIn accepts a string, outputs a Uint8Array.
    // dec accepts a Uint8Array, outputs a string.
    // encOut accepts a string, outputs a Uint8Array.
    const encIn = new TextEncoderStream();
    const dec = new TextDecoderStream();
    const encOut = new TextEncoderStream();

    const mid = encIn.readable.pipeThrough(dec);
    const end = mid.pipeThrough(encOut);

    const writer = encIn.writable.getWriter();
    await writer.write('hello');
    await writer.close();

    const reader = end.getReader();
    const result = await reader.read();

    const decoder = new TextDecoder();
    strictEqual(decoder.decode(result.value), 'hello');
  },
};

// Test TextEncoderStream accepts anything that can be coerced to a string
export const textEncoderStreamTypes = {
  async test() {
    const decoder = new TextDecoder();
    const enc = new TextEncoderStream();
    const writer = enc.writable.getWriter();
    const reader = enc.readable.getReader();

    // Write all values then read them - need to interleave writes and reads properly
    // by using Promise.all to avoid deadlocks from backpressure

    // Test undefined
    {
      const [, result] = await Promise.all([
        writer.write(undefined),
        reader.read(),
      ]);
      strictEqual(decoder.decode(result.value), 'undefined');
    }

    // Test numbers
    {
      const [, result] = await Promise.all([writer.write(1), reader.read()]);
      strictEqual(decoder.decode(result.value), '1');
    }

    // Test objects
    {
      const [, result] = await Promise.all([writer.write({}), reader.read()]);
      strictEqual(decoder.decode(result.value), '[object Object]');
    }

    await writer.close();
  },
};

// Test TextDecoderStream with Big5 encoding
export const textDecoderStreamBig5 = {
  async test() {
    const dec = new TextDecoderStream('big5');
    const input = [0xa4, 0xa4, 0xb0, 0xea, 0xa4, 0x48];
    const check = new Uint8Array([
      0xe4, 0xb8, 0xad, 0xe5, 0x9c, 0x8b, 0xe4, 0xba, 0xba,
    ]);

    const writer = dec.writable.getWriter();

    for (const byte of input) {
      writer.write(new Uint8Array([byte]));
    }
    writer.close();

    let result = '';
    for await (const chunk of dec.readable) {
      result += chunk;
    }

    strictEqual(result, '中國人');

    const decoder = new TextDecoder('utf8');
    strictEqual(result, decoder.decode(check));
  },
};

// Test TextEncoderStream encoding property
export const textEncoderStreamEncoding = {
  test() {
    const enc = new TextEncoderStream();
    strictEqual(enc.encoding, 'utf-8');
  },
};

// Test TextDecoderStream with various encodings
export const textDecoderStreamEncodings = {
  test() {
    // Test UTF-16
    const stream = new TextDecoderStream('utf-16', {
      fatal: true,
      ignoreBOM: true,
    });
    strictEqual(stream.encoding, 'utf-16le');
    strictEqual(stream.fatal, true);
    strictEqual(stream.ignoreBOM, true);
  },
};

// =============================================================================
// Porting status from edgeworker/src/edgeworker/api-tests/streams/encoding.ew-test
//
// PORTED (can be deleted from ew-test):
// - textencoder-stream-roundtrip: Ported as textEncoderDecoderPipeline
// - textencoder-stream-types: Ported as textEncoderStreamTypes
// - textdecoder-stream-big5: Ported as textDecoderStreamBig5
// - textencoder-stream-encoding: Ported as textEncoderStreamEncoding
// - textdecoder-stream-encodings: Ported as textDecoderStreamEncodings
//
// All tests from encoding.ew-test have been fully ported.
// =============================================================================
