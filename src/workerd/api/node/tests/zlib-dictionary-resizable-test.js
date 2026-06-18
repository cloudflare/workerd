// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-82:
// ZlibContext::initialize stored a non-owning view into a resizable
// ArrayBuffer BackingStore as the dictionary. If the buffer was resized
// to 0 before the first write (which lazily calls setDictionary), zlib
// would read into PROT_NONE pages and SIGSEGV the process.
import assert from 'node:assert';
import { Buffer } from 'node:buffer';
import zlib from 'node:zlib';

export const regression_AUTOVULN_CLOUDFLARE_WORKERD_82 = {
  async test() {
    // Create a resizable ArrayBuffer and fill it with non-zero data so
    // zlib adler32 actually reads the dictionary bytes.
    const ab = new ArrayBuffer(1024, { maxByteLength: 1024 });
    const view = new Uint8Array(ab);
    for (let i = 0; i < view.length; i++) {
      view[i] = i & 0xff || 1;
    }

    // Pass the resizable-backed view as the dictionary to Deflate.
    const d = zlib.createDeflate({ dictionary: view });

    // Shrink the backing ArrayBuffer to 0 before the first write.
    // Pre-patch this would cause setDictionary to read PROT_NONE pages.
    ab.resize(0);

    // The first write triggers lazy zlib init and setDictionary.
    // Post-patch this must succeed (the dictionary was deep-copied).
    // Pre-patch this would SIGSEGV the process.
    const compressed = await new Promise((resolve, reject) => {
      const chunks = [];
      d.on('data', (chunk) => chunks.push(chunk));
      d.on('end', () => resolve(Buffer.concat(chunks)));
      d.on('error', (err) => reject(err));
      d.write(Buffer.from('hello world'));
      d.end();
    });

    assert.ok(compressed.length > 0, 'should produce compressed output');

    // Verify the compressed data can be inflated back with the same
    // dictionary (using a fresh, non-resizable copy of the original).
    const dictCopy = Buffer.alloc(1024);
    for (let i = 0; i < 1024; i++) {
      dictCopy[i] = i & 0xff || 1;
    }

    const decompressed = await new Promise((resolve, reject) => {
      const chunks = [];
      const inf = zlib.createInflate({ dictionary: dictCopy });
      inf.on('data', (chunk) => chunks.push(chunk));
      inf.on('end', () => resolve(Buffer.concat(chunks)));
      inf.on('error', (err) => reject(err));
      inf.write(compressed);
      inf.end();
    });

    assert.strictEqual(
      decompressed.toString(),
      'hello world',
      'round-trip deflate+inflate with dictionary must preserve data'
    );
  },
};
