// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// HTMLRewriter.transform() over a document, read with arrayBuffer(): no
// handlers, an element handler, and a text handler, over a runtime-produced
// body and over a JavaScript stream.

import { chunkedValueStream, expectEqual, sizeLabel } from 'helpers';

// About `size` bytes of markup, made of repeated items, each with a link and
// a paragraph. Returns the encoded document with its number of links and
// characters of paragraph text, which the cases check the handlers against.
function documentOf(size) {
  const items = [];
  let length = 0;
  let paragraphText = 0;
  for (let i = 0; length < size; i++) {
    const text = `A short description of item ${i}.`;
    const item =
      `<div class="item"><a href="/item/${i}">Item ${i}</a>` +
      `<p>${text}</p></div>\n`;
    items.push(item);
    length += item.length;
    paragraphText += text.length;
  }
  const bytes = new TextEncoder().encode(
    `<!doctype html><html><body>\n${items.join('')}</body></html>\n`
  );
  return { bytes, links: items.length, paragraphText };
}

// What the element handler adds to each link.
const REL = ' rel="nofollow"';

// `make()` returns a rewriter and a function returning how much its handlers
// saw; `expected(document)` is what rewriting `document` produces.
const HANDLERS = {
  'no handlers': {
    make: () => ({ rewriter: new HTMLRewriter(), seen: () => 0 }),
    expected: (document) => ({ bytes: document.bytes.length, seen: 0 }),
  },
  'element handler': {
    make: () => {
      let elements = 0;
      const rewriter = new HTMLRewriter().on('a', {
        element(element) {
          element.setAttribute('rel', 'nofollow');
          elements++;
        },
      });
      return { rewriter, seen: () => elements };
    },
    expected: (document) => ({
      bytes: document.bytes.length + document.links * REL.length,
      seen: document.links,
    }),
  },
  'text handler': {
    make: () => {
      let characters = 0;
      const rewriter = new HTMLRewriter().on('p', {
        text(text) {
          characters += text.text.length;
        },
      });
      return { rewriter, seen: () => characters };
    },
    expected: (document) => ({
      bytes: document.bytes.length,
      seen: document.paragraphText,
    }),
  },
};

// Rewrites `body` with `handler`'s rewriter, and checks the output's size
// and what the handlers saw against `expected`.
async function rewrite(handler, body, expected) {
  const { rewriter, seen } = handler.make();
  const response = rewriter.transform(new Response(body));
  const bytes = (await response.arrayBuffer()).byteLength;
  expectEqual(bytes, expected.bytes, 'bytes rewritten');
  expectEqual(seen(), expected.seen, 'seen by the handlers');
}

export const htmlRewriter = {
  bench(b) {
    for (const size of [4096, 65536]) {
      const document = documentOf(size);
      for (const [name, handler] of Object.entries(HANDLERS)) {
        const expected = handler.expected(document);
        b.run(`${name}, ${sizeLabel(size)} document`, () =>
          rewrite(handler, document.bytes, expected)
        );
      }
    }
    const document = documentOf(65536);
    const handler = HANDLERS['element handler'];
    const expected = handler.expected(document);
    b.run('element handler, 64 KiB document from a value stream', () =>
      rewrite(handler, chunkedValueStream(document.bytes, 4096), expected)
    );
  },
};
