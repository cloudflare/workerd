// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, throws } from 'node:assert';
import { inspect } from 'node:util';

export const test1 = {
  async test(ctrl, env, ctx) {
    let blob = new Blob(['foo', new TextEncoder().encode('bar'), 'baz']);
    strictEqual(await blob.text(), 'foobarbaz');
    strictEqual(
      new TextDecoder().decode(await blob.arrayBuffer()),
      'foobarbaz'
    );
    strictEqual(blob.type, '');

    let blob2 = new Blob(['xx', blob, 'yy', blob], {
      type: 'application/whatever',
    });
    strictEqual(await blob2.text(), 'xxfoobarbazyyfoobarbaz');
    strictEqual(blob2.type, 'application/whatever');

    let blob3 = new Blob();
    strictEqual(await blob3.text(), '');

    let slice = blob2.slice(5, 16);
    strictEqual(await slice.text(), 'barbazyyfoo');
    strictEqual(slice.type, '');

    let slice2 = slice.slice(-5, 1234, 'type/type');
    strictEqual(await slice2.text(), 'yyfoo');
    strictEqual(slice2.type, 'type/type');

    strictEqual(await blob2.slice(5).text(), 'barbazyyfoobarbaz');
    strictEqual(await blob2.slice().text(), 'xxfoobarbazyyfoobarbaz');
    strictEqual(await blob2.slice(3, 1).text(), '');

    {
      let stream = blob.stream();
      let reader = stream.getReader();
      let readResult = await reader.read();
      strictEqual(readResult.done, false);
      strictEqual(new TextDecoder().decode(readResult.value), 'foobarbaz');
      readResult = await reader.read();
      strictEqual(readResult.value, undefined);
      strictEqual(readResult.done, true);
      reader.releaseLock();
    }

    let before = Date.now();

    let file = new File([blob, 'qux'], 'filename.txt');
    strictEqual(file instanceof Blob, true);
    strictEqual(await file.text(), 'foobarbazqux');
    strictEqual(file.name, 'filename.txt');
    strictEqual(file.type, '');
    if (file.lastModified < before || file.lastModified > Date.now()) {
      throw new Error('incorrect lastModified');
    }

    let file2 = new File(['corge', file], 'file2', {
      type: 'text/foo',
      lastModified: 123,
    });
    strictEqual(await file2.text(), 'corgefoobarbazqux');
    strictEqual(file2.name, 'file2');
    strictEqual(file2.type, 'text/foo');
    strictEqual(file2.lastModified, 123);

    try {
      new Blob(['foo'], { endings: 'native' });
      throw new Error("use of 'endings' should throw");
    } catch (err) {
      if (
        !err.message.includes(
          "The 'endings' field on 'Options' is not implemented."
        )
      ) {
        throw err;
      }
    }

    // Test type normalization.
    strictEqual(new Blob([], { type: 'FoO/bAr' }).type, 'foo/bar');
    strictEqual(new Blob([], { type: 'FoO\u0019/bAr' }).type, '');
    strictEqual(new Blob([], { type: 'FoO\u0020/bAr' }).type, 'foo /bar');
    strictEqual(new Blob([], { type: 'FoO\u007e/bAr' }).type, 'foo\u007e/bar');
    strictEqual(new Blob([], { type: 'FoO\u0080/bAr' }).type, '');
    strictEqual(new File([], 'foo.txt', { type: 'FoO/bAr' }).type, 'foo/bar');
    strictEqual(blob2.slice(1, 2, 'FoO/bAr').type, 'foo/bar');
  },
};

export const test2 = {
  async test(ctrl, env, ctx) {
    // This test verifies that a Blob created from a request/response properly reflects
    // the content and content-type specified by the request/response.
    const res = await env['request-blob'].fetch(
      'http://example.org/blob-request',
      {
        method: 'POST',
        body: 'abcd1234',
        headers: { 'content-type': 'some/type' },
      }
    );

    strictEqual(res.headers.get('content-type'), 'return/type');
    strictEqual(await res.text(), 'foobarbaz');
  },
  async doTest(request) {
    let b = await request.blob();
    strictEqual(await b.text(), 'abcd1234');
    strictEqual(b.type, 'some/type');

    // Quick check that content-type header is correctly not set when the blob type is an empty
    // string.
    strictEqual(
      new Response(new Blob(['typeful'], { type: 'foo' })).headers.has(
        'Content-Type'
      ),
      true
    );
    strictEqual(
      new Response(new Blob(['typeless'])).headers.has('Content-Type'),
      false
    );

    return new Response(new Blob(['foobar', 'baz'], { type: 'return/type' }));
  },
};

export default {
  async fetch(request) {
    if (request.url.endsWith('/blob-request')) {
      return test2.doTest(request);
    } else {
      throw new Error('Unexpected test request');
    }
  },
};

export const testInspect = {
  async test(ctrl, env, ctx) {
    const blob = new Blob(['abc'], { type: 'text/plain' });
    strictEqual(inspect(blob), "Blob { size: 3, type: 'text/plain' }");

    const file = new File(['1'], 'file.txt', {
      type: 'text/plain',
      lastModified: 1000,
    });
    strictEqual(
      inspect(file),
      "File { name: 'file.txt', lastModified: 1000, size: 1, type: 'text/plain' }"
    );
  },
};

export const overLarge = {
  test() {
    const blob1 = new Blob([new ArrayBuffer(128 * 1024 * 1024)]);

    throws(
      () => {
        new Blob([new ArrayBuffer(128 * 1024 * 1024 + 1)]);
      },
      {
        message: 'Blob size 134217729 exceeds limit 134217728',
        name: 'RangeError',
      }
    );

    throws(
      () => {
        new Blob([' ', blob1]);
      },
      {
        message: 'Blob size 134217729 exceeds limit 134217728',
        name: 'RangeError',
      }
    );
  },
};

// Test that when a resizable ArrayBuffer is shrunk during Blob construction
// (via a later element's Symbol.toPrimitive triggering the resize), the data
// is packed tightly — the shrunk buffer's actual bytes come first, followed
// by subsequent parts, with no zero-filled gaps in the middle.
export const resizeDuringConstruction = {
  async test() {
    const buffer = new ArrayBuffer(1024, { maxByteLength: 2048 });
    const view = new Uint8Array(buffer);
    view.fill(0x41); // fill with 'A'

    // When JSG converts this to a string, it runs Symbol.toPrimitive which
    // shrinks the buffer. The TypedArray is length-tracking so its byteLength
    // drops to 8 before concat() ever reads it.
    const resizer = {
      [Symbol.toPrimitive]() {
        buffer.resize(8);
        return 'B'.repeat(16);
      },
    };

    const blob = new Blob([view, resizer]);
    const bytes = await blob.bytes();

    // view shrank to 8 bytes of 'A', resizer produced 16 bytes of 'B'.
    // Data should be packed tightly: 8 + 16 = 24.
    strictEqual(bytes.length, 24);
    // First 8 bytes are 0x41 ('A') from the shrunk view
    for (let i = 0; i < 8; i++) {
      strictEqual(bytes[i], 0x41, `byte ${i} should be 0x41`);
    }
    // Next 16 bytes are 0x42 ('B') from the resizer's string
    for (let i = 8; i < 24; i++) {
      strictEqual(bytes[i], 0x42, `byte ${i} should be 0x42`);
    }
  },
};

// Same pattern but the buffer is grown instead of shrunk. Because the resize
// happens during argument coercion (before concat() runs), the grown view is
// visible to concat() and its full contents are included in the Blob.
export const resizeGrowDuringConstruction = {
  async test() {
    const buffer = new ArrayBuffer(10, { maxByteLength: 64 });
    const view = new Uint8Array(buffer);
    view.fill(0x41); // fill with 'A'

    const resizer = {
      [Symbol.toPrimitive]() {
        buffer.resize(20);
        new Uint8Array(buffer, 10).fill(0x43); // fill grown region with 'C'
        return 'B'.repeat(10);
      },
    };

    const blob = new Blob([view, resizer]);
    const bytes = await blob.bytes();

    // view grew to 20 during conversion, so concat sees 20 + 10 = 30.
    strictEqual(bytes.length, 30);
    // First 10 bytes: original 0x41 ('A')
    for (let i = 0; i < 10; i++) {
      strictEqual(bytes[i], 0x41, `byte ${i} should be 0x41`);
    }
    // Next 10 bytes: grown region 0x43 ('C')
    for (let i = 10; i < 20; i++) {
      strictEqual(bytes[i], 0x43, `byte ${i} should be 0x43`);
    }
    // Last 10 bytes: 0x42 ('B') from the resizer string
    for (let i = 20; i < 30; i++) {
      strictEqual(bytes[i], 0x42, `byte ${i} should be 0x42`);
    }
  },
};

// Regression tests for the index desync bug in concat() cachedPartSizes.
// When empty text or blob parts appeared before non-empty parts, the second
// pass index fell behind the first pass, causing wrong cachedPartSizes reads.
export const mixedEmptyParts = {
  async test() {
    // Empty string before buffer
    {
      const buf = new Uint8Array([1, 2, 3]).buffer;
      const blob = new Blob(['', buf]);
      strictEqual(blob.size, 3);
      const result = new Uint8Array(await blob.arrayBuffer());
      strictEqual(result[0], 1);
      strictEqual(result[1], 2);
      strictEqual(result[2], 3);
    }

    // Empty blob before string
    {
      const blob = new Blob([new Blob([]), 'hello']);
      strictEqual(blob.size, 5);
      strictEqual(await blob.text(), 'hello');
    }

    // Buffer, empty string, string
    {
      const buf = new Uint8Array([65, 66]).buffer; // "AB"
      const blob = new Blob([buf, '', 'CD']);
      strictEqual(blob.size, 4);
      strictEqual(await blob.text(), 'ABCD');
    }

    // Multiple empties before data
    {
      const blob = new Blob(['', '', new Uint8Array([1, 2]).buffer, 'abc']);
      strictEqual(blob.size, 5);
      const bytes = new Uint8Array(await blob.arrayBuffer());
      strictEqual(bytes[0], 1);
      strictEqual(bytes[1], 2);
      strictEqual(bytes[2], 97); // 'a'
      strictEqual(bytes[3], 98); // 'b'
      strictEqual(bytes[4], 99); // 'c'
    }

    // Empty blob between non-empty parts
    {
      const blob = new Blob(['hello', new Blob([]), ' world']);
      strictEqual(blob.size, 11);
      strictEqual(await blob.text(), 'hello world');
    }

    // All empty parts
    {
      const blob = new Blob(['', new Blob([]), new ArrayBuffer(0)]);
      strictEqual(blob.size, 0);
      strictEqual(await blob.text(), '');
    }
  },
};

export const depthTest = {
  async test() {
    let blob = new Blob(['x']);
    const depth = 100_000;

    for (let i = 0; i < depth; i++) {
      blob = blob.slice(0, 1);
    }

    // Force GC to trigger recursive traversal
    gc();

    // Access the blob to ensure it's still valid
    console.log('Blob size:', blob.size);
  },
};
