// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `<esi:include>` with and without html_rewriter_treats_esi_include_as_void_tag, under the C++
// and the TypeScript HTMLRewriter. Each .wd-test cell runs this module in two services, one with
// the flag (ESI_INCLUDE_IS_VOID) and one without, and says which HTMLRewriter it installs
// (USING_TS_REWRITER).

import { strictEqual, deepStrictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

const esiIncludeIsVoid = env.ESI_INCLUDE_IS_VOID === true;

async function rewrite(html, configure) {
  const rewriter = new HTMLRewriter();
  configure(rewriter);
  return rewriter.transform(new Response(html)).text();
}

// Guards against a cell that runs the other HTMLRewriter than it means to, which would make its
// expectations pass for the wrong implementation.
export const runsTheIntendedRewriter = {
  test() {
    const isNative = Function.prototype.toString
      .call(HTMLRewriter)
      .includes('[native code]');
    strictEqual(!isNative, env.USING_TS_REWRITER === true);
    strictEqual(
      globalThis.Cloudflare.compatibilityFlags[
        'html_rewriter_treats_esi_include_as_void_tag'
      ],
      esiIncludeIsVoid
    );
  },
};

export const esiIncludeIsMatchedBySelector = {
  async test() {
    const seen = [];
    const out = await rewrite('<esi:include src="a.html"/>', (r) =>
      r.on('esi\\:include', {
        element(el) {
          seen.push([el.tagName, el.getAttribute('src')]);
        },
      })
    );
    deepStrictEqual(seen, [['esi:include', 'a.html']]);
    strictEqual(out, '<esi:include src="a.html"/>');
  },
};

export const textAfterAnEsiInclude = {
  async test() {
    // With the flag, the tag has no content, so the text after it is the parent's.
    const inside = [];
    await rewrite('<div><esi:include src="a">x</esi:include>y</div>', (r) =>
      r.on('esi\\:include', {
        text(t) {
          if (t.text !== '') inside.push(t.text);
        },
      })
    );
    deepStrictEqual(inside, esiIncludeIsVoid ? [] : ['x']);
  },
};

export const replaceAnEsiInclude = {
  async test() {
    const out = await rewrite(
      '<p><esi:include src="a">fallback</esi:include>after</p>',
      (r) =>
        r.on('esi\\:include', {
          element(el) {
            el.replace(`<b>${el.getAttribute('src')}</b>`, { html: true });
          },
        })
    );
    strictEqual(
      out,
      esiIncludeIsVoid
        ? '<p><b>a</b>fallback</esi:include>after</p>'
        : '<p><b>a</b>after</p>'
    );
  },
};

export const replaceASelfClosingEsiInclude = {
  async test() {
    const out = await rewrite(
      '<p><esi:include src="a"/>after</p><p>next</p>',
      (r) =>
        r.on('esi\\:include', {
          element(el) {
            el.replace('<b>a</b>', { html: true });
          },
        })
    );
    // Without the flag, `/>` on a tag that is not void is ignored, as HTML has it, so the element
    // runs on and its replacement drops what follows it.
    strictEqual(
      out,
      esiIncludeIsVoid
        ? '<p><b>a</b>after</p><p>next</p>'
        : '<p><b>a</b><p>next</p>'
    );
  },
};

export const onEndTagOfAnEsiInclude = {
  async test() {
    let error;
    const out = await rewrite('<esi:include src="a">x</esi:include>', (r) =>
      r.on('esi\\:include', {
        element(el) {
          try {
            el.onEndTag((end) => {
              end.before('!');
            });
          } catch (e) {
            error = e;
          }
        },
      })
    );
    if (esiIncludeIsVoid) {
      strictEqual(error?.name, 'TypeError');
      strictEqual(error.message, 'Parser error: No end tag.');
      strictEqual(out, '<esi:include src="a">x</esi:include>');
    } else {
      strictEqual(error, undefined);
      strictEqual(out, '<esi:include src="a">x!</esi:include>');
    }
  },
};

export const esiIncludeSplitAcrossChunks = {
  async test() {
    const chunks = ['<p><esi:inc', 'lude src="a"', '>x</esi:', 'include>y</p>'];
    const encoder = new TextEncoder();
    const { readable, writable } = new IdentityTransformStream();
    const out = new HTMLRewriter()
      .on('esi\\:include', {
        element(el) {
          el.replace('<b>a</b>', { html: true });
        },
      })
      .transform(new Response(readable))
      .text();
    const writer = writable.getWriter();
    for (const chunk of chunks) await writer.write(encoder.encode(chunk));
    await writer.close();
    strictEqual(
      await out,
      esiIncludeIsVoid ? '<p><b>a</b>x</esi:include>y</p>' : '<p><b>a</b>y</p>'
    );
  },
};

// The flag makes only `esi:include` void, not other tags in the namespace.
export const otherEsiTagsKeepTheirContent = {
  async test() {
    const out = await rewrite(
      '<esi:remove>fallback</esi:remove><esi:include src="a">x</esi:include>',
      (r) =>
        r.on('esi\\:remove', {
          element(el) {
            el.remove();
          },
        })
    );
    strictEqual(out, '<esi:include src="a">x</esi:include>');
  },
};
