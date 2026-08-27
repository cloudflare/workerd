// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextEncoderStream/TextDecoderStream composed with Response and Request.
// The encoder's readable is a byte source usable as a body; the decoder's
// readable yields STRINGS, so consuming it as a body fails.

import { strictEqual, ok, rejects } from 'node:assert';

export const encoderResponseBody = {
  async test() {
    const tes = new TextEncoderStream();
    const resp = new Response(tes.readable);
    // The body is the very same stream object: not wrapped, consumed, or
    // locked by Response construction.
    strictEqual(resp.body, tes.readable);
    ok(!tes.readable.locked);

    const writer = tes.writable.getWriter();
    const textPromise = resp.text();
    await writer.write('héllo ');
    await writer.write('wörld');
    await writer.close();
    strictEqual(await textPromise, 'héllo wörld');
  },
};

export const encoderRequestBody = {
  async test() {
    const tes = new TextEncoderStream();
    const req = new Request('https://example.com/', {
      method: 'POST',
      body: tes.readable,
    });
    strictEqual(req.body, tes.readable);

    const writer = tes.writable.getWriter();
    const textPromise = req.text();
    await writer.write('ping');
    await writer.close();
    strictEqual(await textPromise, 'ping');
  },
};

export const decoderReadableAsBodyRejectsText = {
  async test() {
    // The decoder's readable delivers strings; body consumption requires
    // bytes and rejects, while the writer side settles normally (the
    // consumer drained the chunk before failing on its type).
    const tds = new TextDecoderStream();
    const resp = new Response(tds.readable);
    const writer = tds.writable.getWriter();
    // All three promises are created (and handled) up front: the failing
    // consumer may cancel the readable, settling the writer-side promises
    // with it.
    const textExpectation = rejects(resp.text(), (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(err.message, 'This ReadableStream did not return bytes.');
      return true;
    });
    const writePromise = writer.write(new TextEncoder().encode('abc'));
    const closePromise = writer.close();
    await textExpectation;
    await writePromise;
    await closePromise;
  },
};

export const responseBodyThroughDecoder = {
  async test() {
    const decoded = new Response('déjà vu').body.pipeThrough(
      new TextDecoderStream()
    );
    let result = '';
    for await (const chunk of decoded) {
      strictEqual(typeof chunk, 'string');
      result += chunk;
    }
    strictEqual(result, 'déjà vu');
  },
};

export const byteBodyIntoEncoderCoercesChunks = {
  async test() {
    // Piping BYTES into the encoder's string-accepting writable is a
    // footgun, not an error: each Uint8Array chunk is ToString-coerced to
    // its comma-joined decimal form and that text is encoded.
    const tes = new TextEncoderStream();
    const pipePromise = new Response('xy').body.pipeTo(tes.writable);
    let result = '';
    const dec = new TextDecoder();
    for await (const chunk of tes.readable) {
      result += dec.decode(chunk, { stream: true });
    }
    await pipePromise;
    strictEqual(result, '120,121'); // the byte VALUES of 'x' and 'y'
  },
};

export const largeEncodedResponseBody = {
  async test() {
    // A large mixed-width payload chunked at arbitrary boundaries: slicing
    // may split surrogate pairs across writes, which the encoder re-pairs,
    // so the consumed text is identical to the input.
    const payload = 'a€😀б¢\n'.repeat(40_000); // 240k code units
    const tes = new TextEncoderStream();
    const textPromise = new Response(tes.readable).text();
    const writer = tes.writable.getWriter();
    const chunkSize = 8191; // odd size to vary the split positions
    for (let i = 0; i < payload.length; i += chunkSize) {
      await writer.write(payload.slice(i, i + chunkSize));
    }
    await writer.close();
    strictEqual(await textPromise, payload);
  },
};
