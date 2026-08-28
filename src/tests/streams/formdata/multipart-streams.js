// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// FormData × streams: multipart parsing FROM streamed bodies (with
// chunk boundaries deliberately split inside multipart markers) and
// FormData serialized INTO a body consumed as a stream.

import { strictEqual, ok, rejects } from 'node:assert';

const enc = new TextEncoder();
const BOUNDARY = 'streamsuiteboundary';

function multipartBody(fields) {
  const parts = [];
  for (const [name, value, filename] of fields) {
    parts.push(`--${BOUNDARY}\r\n`);
    if (filename !== undefined) {
      parts.push(
        `Content-Disposition: form-data; name="${name}"; filename="${filename}"\r\n` +
          'Content-Type: application/octet-stream\r\n\r\n'
      );
    } else {
      parts.push(`Content-Disposition: form-data; name="${name}"\r\n\r\n`);
    }
    parts.push(`${value}\r\n`);
  }
  parts.push(`--${BOUNDARY}--`);
  return parts.join('');
}

function requestWithStreamedBody(body, chunkAt) {
  // Split the body at the given offsets, streaming each slice as its
  // own byte chunk.
  const offsets = [0, ...chunkAt, body.length];
  const chunks = [];
  for (let i = 0; i + 1 < offsets.length; i++) {
    chunks.push(body.slice(offsets[i], offsets[i + 1]));
  }
  let i = 0;
  const rs = new ReadableStream({
    type: 'bytes',
    pull(c) {
      c.enqueue(enc.encode(chunks[i++]));
      if (i >= chunks.length) c.close();
    },
  });
  return new Request('http://example.org/', {
    method: 'POST',
    body: rs,
    headers: {
      'content-type': `multipart/form-data; boundary=${BOUNDARY}`,
    },
  });
}

// Baseline: the whole multipart body in ONE stream chunk.
export const parseMultipartSingleChunk = {
  async test() {
    const body = multipartBody([
      ['alpha', 'one'],
      ['beta', 'two'],
    ]);
    const form = await requestWithStreamedBody(body, []).formData();
    strictEqual(form.get('alpha'), 'one');
    strictEqual(form.get('beta'), 'two');
  },
};

// Chunk boundaries INSIDE the multipart markers: mid-boundary,
// mid-header, and mid-value splits must all reassemble.
export const parseMultipartAwkwardChunkSplits = {
  async test() {
    const body = multipartBody([
      ['alpha', 'one'],
      ['beta', 'two two two'],
      ['gamma', 'three'],
    ]);
    // Split inside the first boundary marker, inside a
    // Content-Disposition header, and inside a value.
    const splits = [
      3, // inside '--streamsuiteboundary'
      body.indexOf('name="beta"') + 4, // inside a header
      body.indexOf('two two two') + 5, // inside a value
      body.lastIndexOf('--') + 1, // inside the closing marker
    ].sort((a, b) => a - b);
    const form = await requestWithStreamedBody(body, splits).formData();
    strictEqual(form.get('alpha'), 'one');
    strictEqual(form.get('beta'), 'two two two');
    strictEqual(form.get('gamma'), 'three');
  },
};

// EVERY byte its own chunk (worst-case reassembly) over a small form.
export const parseMultipartBytewiseChunks = {
  async test() {
    const body = multipartBody([['key', 'val']]);
    const splits = Array.from({ length: body.length - 1 }, (_, i) => i + 1);
    const form = await requestWithStreamedBody(body, splits).formData();
    strictEqual(form.get('key'), 'val');
  },
};

// File entries parsed out of a streamed multipart body, with the file
// content read back via file.text() (a Blob-backed re-stream).
export const parseFilesFromStreamedMultipart = {
  async test() {
    const body = multipartBody([
      ['doc', 'file-content-here', 'doc.txt'],
      ['plain', 'not-a-file'],
    ]);
    const form = await requestWithStreamedBody(body, [
      body.indexOf('file-content') + 6,
    ]).formData();
    const file = form.get('doc');
    ok(file instanceof File);
    strictEqual(file.name, 'doc.txt');
    strictEqual(await file.text(), 'file-content-here');
    strictEqual(form.get('plain'), 'not-a-file');
  },
};

// An IdentityTransformStream body fed concurrently parses the same way.
export const parseMultipartFromIdentityStream = {
  async test() {
    const body = multipartBody([['field', 'value']]);
    const its = new IdentityTransformStream();
    const request = new Request('http://example.org/', {
      method: 'POST',
      body: its.readable,
      headers: {
        'content-type': `multipart/form-data; boundary=${BOUNDARY}`,
      },
    });
    const formP = request.formData();
    const writer = its.writable.getWriter();
    const mid = Math.floor(body.length / 2);
    await writer.write(enc.encode(body.slice(0, mid)));
    await writer.write(enc.encode(body.slice(mid)));
    await writer.close();
    const form = await formP;
    strictEqual(form.get('field'), 'value');
  },
};

// A LARGE streamed multipart body: 100 fields plus a 256 KiB file.
export const parseLargeStreamedMultipart = {
  async test() {
    const fields = Array.from({ length: 100 }, (_, i) => [
      `field${i}`,
      `value-${i}`,
    ]);
    const bigContent = 'x'.repeat(256 * 1024);
    fields.push(['big', bigContent, 'big.bin']);
    const body = multipartBody(fields);
    const splits = [];
    for (let at = 8 * 1024; at < body.length; at += 8 * 1024) splits.push(at);
    const form = await requestWithStreamedBody(body, splits).formData();
    strictEqual(form.get('field0'), 'value-0');
    strictEqual(form.get('field99'), 'value-99');
    const file = form.get('big');
    ok(file instanceof File);
    strictEqual((await file.text()).length, bigContent.length);
  },
};

// An erroring body stream rejects formData() parsing.
export const erroringBodyRejectsFormData = {
  async test() {
    const boom = new Error('boom');
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode(`--${BOUNDARY}\r\n`));
      },
      pull(c) {
        c.error(boom);
      },
    });
    const request = new Request('http://example.org/', {
      method: 'POST',
      body: rs,
      headers: {
        'content-type': `multipart/form-data; boundary=${BOUNDARY}`,
      },
    });
    await rejects(
      request.formData(),
      (e) => e === boom || /boom/.test(e.message)
    );
  },
};

// FormData serialized INTO a body: the response body is a readable
// stream whose content reparses to the same form.
export const serializedFormDataBodyRoundTrips = {
  async test() {
    const form = new FormData();
    form.append('alpha', 'one');
    form.append('beta', 'two');
    form.append('file', new File(['file-bytes'], 'f.txt'));
    const response = new Response(form);
    ok(response.body instanceof ReadableStream);
    const reader = response.body.getReader();
    const parts = [];
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      parts.push(value);
    }
    const total = parts.reduce((n, p) => n + p.byteLength, 0);
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const part of parts) {
      bytes.set(part, offset);
      offset += part.byteLength;
    }
    // Reparse through a fresh Response carrying the original
    // content-type (with its generated boundary).
    const reparsed = await new Response(bytes, {
      headers: { 'content-type': response.headers.get('content-type') },
    }).formData();
    strictEqual(reparsed.get('alpha'), 'one');
    strictEqual(reparsed.get('beta'), 'two');
    strictEqual(await reparsed.get('file').text(), 'file-bytes');
  },
};
