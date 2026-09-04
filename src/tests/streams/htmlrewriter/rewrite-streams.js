// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// HTMLRewriter consuming and producing stream bodies: the rewriter
// reads whatever body shape the response carries, parses across
// arbitrary chunk boundaries, and its OUTPUT is itself a readable
// stream. Content insertion FROM a ReadableStream (the streaming
// replacement extension) reads that stream through the same machinery.

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();

function chunkedByteSource(chunks) {
  let i = 0;
  return new ReadableStream({
    type: 'bytes',
    pull(c) {
      c.enqueue(enc.encode(chunks[i++]));
      if (i >= chunks.length) c.close();
    },
  });
}

// Passthrough (no handlers) over a JS VALUE stream body.
export const passthroughJsValueStream = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('<html><body>'));
        c.enqueue(enc.encode('hello</body></html>'));
        c.close();
      },
    });
    const result = await new HTMLRewriter().transform(new Response(rs)).text();
    strictEqual(result, '<html><body>hello</body></html>');
  },
};

// Passthrough over a JS BYTE stream body.
export const passthroughJsByteStream = {
  async test() {
    const result = await new HTMLRewriter()
      .transform(
        new Response(chunkedByteSource(['<html><body>hi', '</body></html>']))
      )
      .text();
    strictEqual(result, '<html><body>hi</body></html>');
  },
};

// A handler over a document whose chunks split MID-TAG: the parser
// must reassemble the tag across stream chunk boundaries.
export const handlerAcrossChunkBoundaries = {
  async test() {
    const seen = [];
    const result = await new HTMLRewriter()
      .on('div', {
        element(element) {
          seen.push(element.getAttribute('id'));
          element.setAttribute('data-seen', 'yes');
        },
      })
      .transform(
        new Response(
          chunkedByteSource(['<di', 'v id="a">x</d', 'iv><div id="b">y</div>'])
        )
      )
      .text();
    strictEqual(seen.join(','), 'a,b');
    strictEqual(
      result,
      '<div id="a" data-seen="yes">x</div><div id="b" data-seen="yes">y</div>'
    );
  },
};

// The rewriter's OUTPUT body is a ReadableStream consumable
// incrementally with a reader.
export const rewrittenBodyIsReadableStream = {
  async test() {
    const transformed = new HTMLRewriter()
      .on('b', {
        element(element) {
          element.replace('strong', { html: false });
        },
      })
      .transform(new Response(chunkedByteSource(['<p><b>x</b>', '</p>'])));
    ok(transformed.body instanceof ReadableStream);
    const reader = transformed.body.getReader();
    let total = '';
    const dec = new TextDecoder();
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      total += dec.decode(value, { stream: true });
    }
    total += dec.decode();
    strictEqual(total, '<p>strong</p>');
  },
};

// Streaming replacement: element content inserted FROM a
// ReadableStream.
export const contentFromReadableStream = {
  async test() {
    const contentStream = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('streamed '));
        c.enqueue(enc.encode('content'));
        c.close();
      },
    });
    const result = await new HTMLRewriter()
      .on('div', {
        element(element) {
          element.replace(contentStream);
        },
      })
      .transform(new Response('<div>old</div>'))
      .text();
    strictEqual(result, 'streamed content');
  },
};

// An IdentityTransformStream body fed by a concurrent writer.
export const identityStreamBody = {
  async test() {
    const its = new IdentityTransformStream();
    const resultP = new HTMLRewriter()
      .transform(new Response(its.readable))
      .text();
    const writer = its.writable.getWriter();
    await writer.write(enc.encode('<p>id'));
    await writer.write(enc.encode('entity</p>'));
    await writer.close();
    strictEqual(await resultP, '<p>identity</p>');
  },
};

// Cancelling the transformed body reaches the source lazily: another
// source chunk must wake the parked pump before it forwards the reason.
// The TypeScript implementation then pulls once more than C++; its internal
// pump also reports one unhandled rejection outside the worker's event realm.
export const cancelReachesSourceAfterNextChunk = {
  async test() {
    let cancelReason = 'not-called';
    let pulls = 0;
    let controller;
    const { promise: pumpParked, resolve: resolvePumpParked } =
      Promise.withResolvers();
    const { promise: canceled, resolve: resolveCanceled } =
      Promise.withResolvers();
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
      pull() {
        pulls++;
        if (pulls === 3) resolvePumpParked();
      },
      cancel(reason) {
        cancelReason = String(reason);
        resolveCanceled();
      },
    });
    const transformed = new HTMLRewriter().transform(new Response(rs));
    const reader = transformed.body.getReader();
    controller.enqueue(enc.encode('<p>first</p>'));
    await reader.read();
    await pumpParked;
    await reader.cancel('done early');
    strictEqual(cancelReason, 'not-called');
    const pullsAtCancel = pulls;

    controller.enqueue(enc.encode('<p>second</p>'));
    await canceled;

    strictEqual(cancelReason, 'Error: done early');
    strictEqual(pulls, pullsAtCancel + (usingTsImpl ? 2 : 1));
  },
};

// An erroring source stream rejects the transformed body's
// consumption.
export const erroringSourceRejectsConsumption = {
  async test() {
    const boom = new Error('boom');
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('<p>x'));
      },
      pull(c) {
        c.error(boom);
      },
    });
    await rejects(
      new HTMLRewriter().transform(new Response(rs)).text(),
      (e) => e !== boom && e.name === 'Error' && e.message === 'boom'
    );
  },
};

// A LARGE document (256 KiB of repeated elements) through a counting
// handler, with byte-exact output.
export const largeDocumentThroughHandler = {
  async test() {
    const UNIT = `<div class="item">${'content here'.repeat(20)}</div>`;
    const COUNT = 1024;
    let elements = 0;
    let offset = 0;
    const doc = UNIT.repeat(COUNT);
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        const length = Math.min(16 * 1024, doc.length - offset);
        c.enqueue(enc.encode(doc.slice(offset, offset + length)));
        offset += length;
        if (offset >= doc.length) c.close();
      },
    });
    const result = await new HTMLRewriter()
      .on('div.item', {
        element() {
          elements++;
        },
      })
      .transform(new Response(rs))
      .text();
    strictEqual(elements, COUNT);
    strictEqual(result, doc);
  },
};
