// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// File parts parsed out of a multipart body keep their exact bytes,
// including bytes that are not valid UTF-8, whether the body is a
// serialized FormData, a buffer, or a stream. Decoding the body as UTF-8
// before parsing it would turn each invalid sequence into U+FFFD.

import { deepStrictEqual, ok } from 'node:assert';

const enc = new TextEncoder();
const BOUNDARY = 'streamsuiteboundary';
const CONTENT_TYPE = `multipart/form-data; boundary=${BOUNDARY}`;

// Every byte value, in order.
const EVERY_BYTE = Uint8Array.from({ length: 256 }, (_, i) => i);

function concatBytes(pieces) {
  const total = pieces.reduce((n, piece) => n + piece.byteLength, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const piece of pieces) {
    out.set(piece, offset);
    offset += piece.byteLength;
  }
  return out;
}

// A multipart body holding a single file part with the given content.
function binaryMultipartBody(content) {
  return concatBytes([
    enc.encode(
      `--${BOUNDARY}\r\n` +
        'Content-Disposition: form-data; name="file"; filename="file.bin"\r\n' +
        'Content-Type: application/octet-stream\r\n\r\n'
    ),
    content,
    enc.encode(`\r\n--${BOUNDARY}--`),
  ]);
}

async function fileBytes(form) {
  const file = form.get('file');
  ok(file instanceof File);
  return new Uint8Array(await file.arrayBuffer());
}

export const parseBinaryFileFromFormDataBody = {
  async test() {
    const form = new FormData();
    form.append('file', new File([EVERY_BYTE], 'file.bin'));
    const request = new Request('http://example.org/', {
      method: 'POST',
      body: form,
    });
    deepStrictEqual(await fileBytes(await request.formData()), EVERY_BYTE);
  },
};

export const parseBinaryFileFromBufferedBody = {
  async test() {
    const response = new Response(binaryMultipartBody(EVERY_BYTE), {
      headers: { 'content-type': CONTENT_TYPE },
    });
    deepStrictEqual(await fileBytes(await response.formData()), EVERY_BYTE);
  },
};

export const parseBinaryFileFromStreamedBody = {
  async test() {
    const body = binaryMultipartBody(EVERY_BYTE);
    const chunkSize = 7;
    let offset = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(body.slice(offset, offset + chunkSize));
        offset += chunkSize;
        if (offset >= body.length) c.close();
      },
    });
    const request = new Request('http://example.org/', {
      method: 'POST',
      body: rs,
      headers: { 'content-type': CONTENT_TYPE },
    });
    deepStrictEqual(await fileBytes(await request.formData()), EVERY_BYTE);
  },
};
