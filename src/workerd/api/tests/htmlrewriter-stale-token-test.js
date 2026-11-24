// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A token used after its handler: content and end tag handlers that a stale token is given throw,
// and the rewrite doesn't keep them.

import { deepStrictEqual } from 'node:assert';

const INVALID_TOKEN =
  'This content token is no longer valid. Content tokens are only valid ' +
  'during the execution of the relevant content handler.';

// Gives `token` what `give` makes, in a function of its own so that nothing here keeps it, and
// returns a weak reference to it and what giving it threw.
function giveStale(token, give, make) {
  const value = make();
  const ref = new WeakRef(value);
  try {
    give(token, value);
    return { ref, error: 'no error' };
  } catch (e) {
    return { ref, error: `${e.name}: ${e.message}` };
  }
}

async function rewriteWithStaleToken(give, make) {
  let kept;
  let result;
  const out = await new HTMLRewriter()
    .on('p', {
      element(el) {
        kept = el;
      },
    })
    .on('i', {
      async element() {
        result = giveStale(kept, give, make);
        // A WeakRef keeps its target until the end of the task that made it.
        await scheduler.wait(1);
        gc();
        // The rewrite is still running, so what it keeps is still alive.
        result.collected = result.ref.deref() === undefined;
      },
    })
    .transform(new Response('<p>a</p><i>b</i>'))
    .text();
  return { out, error: result.error, collected: result.collected };
}

export const streamedContentGivenToAStaleTokenIsNotKept = {
  async test() {
    deepStrictEqual(
      await rewriteWithStaleToken(
        (token, stream) => token.replace(stream),
        () => new ReadableStream()
      ),
      {
        out: '<p>a</p><i>b</i>',
        error: `TypeError: ${INVALID_TOKEN}`,
        collected: true,
      }
    );
  },
};

export const anEndTagHandlerGivenToAStaleTokenIsNotKept = {
  async test() {
    deepStrictEqual(
      await rewriteWithStaleToken(
        (token, handler) => token.onEndTag(handler),
        () => () => {}
      ),
      {
        out: '<p>a</p><i>b</i>',
        error: `TypeError: ${INVALID_TOKEN}`,
        collected: true,
      }
    );
  },
};
