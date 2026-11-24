// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The APIs implemented in C++ that make or take a Blob, through
// api/blob-bridge.h, with either implementation. URL.createObjectURL()'s
// brand check is in the URL suite (src/tests/url).

import { deepStrictEqual, strictEqual } from 'node:assert';
import { blobStandardMimeType } from 'which-impl';

export const responseBlob = {
  async test() {
    const response = new Response('hi', {
      headers: { 'content-type': 'Text/HTML' },
    });
    const blob = await response.blob();
    strictEqual(Object.getPrototypeOf(blob), Blob.prototype);
    // Taken from the content type as it is, unless blob_standard_mime_type.
    strictEqual(blob.type, blobStandardMimeType ? 'text/html' : 'Text/HTML');
    strictEqual(await blob.text(), 'hi');
    const empty = await new Response(null).blob();
    strictEqual(empty.size, 0);
  },
};

export const blobBodies = {
  async test() {
    const file = new File(['xyz'], 'f', { type: 'T/X' });
    const response = new Response(file.slice(1, 3, 'A/B'));
    strictEqual(response.headers.get('content-type'), 'a/b');
    strictEqual(await response.text(), 'yz');
    const request = new Request('http://example.com/', {
      method: 'POST',
      body: file,
    });
    strictEqual(request.headers.get('content-type'), 't/x');
    strictEqual(await request.text(), 'xyz');
    // A Blob without a type sets no content type.
    strictEqual(
      new Response(new Blob(['x'])).headers.get('content-type'),
      null
    );
  },
};

export const formDataEntries = {
  async test() {
    const form = new FormData();
    const blob = new Blob(['x'], { type: 'T/X' });
    form.append('blob', blob);
    const fromBlob = form.get('blob');
    strictEqual(Object.getPrototypeOf(fromBlob), File.prototype);
    deepStrictEqual(
      [fromBlob.name, fromBlob.type, fromBlob === blob],
      ['blob', 't/x', false]
    );
    strictEqual(await fromBlob.text(), 'x');

    const file = new File(['y'], 'n', { lastModified: 1 });
    form.append('file', file);
    strictEqual(form.get('file'), file);
    form.append('renamed', file, 'm');
    const renamed = form.get('renamed');
    deepStrictEqual([renamed === file, renamed.name], [false, 'm']);
    strictEqual(await renamed.text(), 'y');

    form.set('blob', blob, 'named');
    strictEqual(form.get('blob').name, 'named');
    strictEqual([...form.values()].filter((v) => v instanceof File).length, 3);
  },
};

export const formDataRoundTrip = {
  async test() {
    const form = new FormData();
    form.append('a', '1');
    form.append(
      'f',
      new File(['contents'], 'name.txt', { type: 'text/plain' })
    );
    const parsed = await new Response(form).formData();
    strictEqual(parsed.get('a'), '1');
    const file = parsed.get('f');
    strictEqual(Object.getPrototypeOf(file), File.prototype);
    deepStrictEqual([file.name, file.type], ['name.txt', 'text/plain']);
    strictEqual(await file.text(), 'contents');
  },
};
