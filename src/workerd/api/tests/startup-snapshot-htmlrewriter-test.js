// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The TypeScript HTMLRewriter across a startup snapshot. The config turns the startup-snapshot
// and ts-html-rewriter autogates on in every variant, so a Worker that uses HTMLRewriter only at
// request time must start from the snapshot: the bootstrap's native half travels with it. A
// rewriter built at the top level holds a parsed selector, which no snapshot carries (nor the C++
// HTMLRewriter's), so that Worker starts as a plain one.

import { strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

const topLevelRewriter =
  MODE === 'top-level'
    ? new HTMLRewriter().on('p', {
        element(element) {
          element.setAttribute('top', '1');
        },
      })
    : null;

async function rewrite() {
  const rewriter =
    topLevelRewriter ??
    new HTMLRewriter().on('p', {
      async element(element) {
        await scheduler.wait(1);
        element.setAttribute('top', '1');
      },
    });
  return rewriter.transform(new Response('<p>x</p>')).text();
}

async function fetch() {
  return Response.json({
    snapshotUsed: unsafe.startedFromStartupSnapshot(),
    // A TypeScript class's source text is not [native code], unlike a JSG class's.
    tsRewriter: !Function.prototype.toString
      .call(HTMLRewriter)
      .includes('[native code]'),
    output: await rewrite(),
  });
}

async function test(ctrl, env) {
  for (const [name, snapshotUsed] of [
    ['request-time', true],
    ['top-level', false],
  ]) {
    const r = await (await env[name].fetch('http://case/')).json();
    strictEqual(r.snapshotUsed, snapshotUsed, `${name}: snapshotUsed`);
    strictEqual(r.tsRewriter, true, `${name}: tsRewriter`);
    strictEqual(r.output, '<p top="1">x</p>', `${name}: output`);
  }
}

export default MODE === 'runner' ? { fetch, test } : { fetch };
