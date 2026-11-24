// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect() of HTMLRewriter and its tokens, and the marker of a runtime class, under the C++
// and the TypeScript HTMLRewriter. The C++ classes are JSG resource types, whose prototypes carry
// the marker and util.inspect()'s formatting hook, so inspect() shows their properties' values.
// The TypeScript classes have neither, so it shows their accessors; this pins that difference,
// which the header of src/per_isolate/htmlrewriter/htmlrewriter.ts lists.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { inspect } from 'node:util';
import { env } from 'cloudflare:workers';

const usingTsRewriter = env.USING_TS_REWRITER === true;
const kInternalClass = Symbol.for('cloudflare:internal-class');

async function inspectTokens() {
  const seen = {};
  const record = (name, token) => {
    seen[name] ??= {
      inspected: inspect(token),
      marked: kInternalClass in token,
    };
  };
  await new HTMLRewriter()
    .on('p', {
      element(el) {
        record('element', el);
        el.onEndTag((end) => record('endTag', end));
      },
      comments(c) {
        record('comment', c);
      },
      text(t) {
        record('text', t);
      },
    })
    .onDocument({
      doctype(d) {
        record('doctype', d);
      },
      end(e) {
        record('end', e);
      },
    })
    .transform(new Response('<!doctype html><p a="1"><!--c-->x</p>'))
    .text();
  record('rewriter', new HTMLRewriter());
  return seen;
}

export const runsTheIntendedRewriter = {
  test() {
    const isNative = Function.prototype.toString
      .call(HTMLRewriter)
      .includes('[native code]');
    strictEqual(!isNative, usingTsRewriter);
  },
};

export const inspectAndMarker = {
  async test() {
    const seen = await inspectTokens();
    const marked = !usingTsRewriter;
    const expected = usingTsRewriter
      ? {
          element:
            'Element {\n  tagName: [Getter/Setter],\n  attributes: [Getter],\n' +
            '  removed: [Getter],\n  namespaceURI: [Getter]\n}',
          endTag: 'EndTag { name: [Getter/Setter] }',
          comment: 'Comment { text: [Getter/Setter], removed: [Getter] }',
          text: 'Text { text: [Getter], lastInTextNode: [Getter], removed: [Getter] }',
          doctype:
            'Doctype { name: [Getter], publicId: [Getter], systemId: [Getter] }',
          end: 'DocumentEnd {}',
          rewriter: 'HTMLRewriter {}',
        }
      : {
          element:
            "Element {\n  namespaceURI: 'http://www.w3.org/1999/xhtml',\n  removed: false,\n" +
            "  attributes: AttributesIterator {},\n  tagName: 'p'\n}",
          endTag: "EndTag { name: 'p' }",
          comment: "Comment { removed: false, text: 'c' }",
          text: "Text { removed: false, lastInTextNode: false, text: 'x' }",
          doctype: "Doctype { systemId: null, publicId: null, name: 'html' }",
          end: 'DocumentEnd {}',
          rewriter: 'HTMLRewriter {}',
        };
    for (const name of Object.keys(expected)) {
      deepStrictEqual(seen[name], { inspected: expected[name], marked }, name);
    }
  },
};
