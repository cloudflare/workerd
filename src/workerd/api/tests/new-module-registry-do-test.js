// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';
import { strictEqual } from 'node:assert';
import { VALUE } from 'helper';

// The static import graph above (a cloudflare: builtin, a node: builtin, and
// a worker-bundle module) is resolved through the new module registry when
// the worker starts.

export class TestActor extends DurableObject {
  async check() {
    // Static bundle imports are visible in actor code.
    strictEqual(VALUE, 42);

    // Dynamic import at request time, inside the actor's context. Under the
    // new module registry this resolves synchronously within the V8 callback
    // and evaluates the module lazily.
    const helper = await import('helper');
    strictEqual(helper.VALUE, 42);
    strictEqual(helper.url, 'file:///bundle/helper');

    // A CommonJS module (with a nested require()) evaluated lazily from
    // actor code.
    const cjs = await import('cjs');
    strictEqual(cjs.default.fromRequire, 42);

    // Non-ESM module types resolve from actor code too.
    const words = await import('words');
    strictEqual(words.default, 'hello from text module');

    // The actor's storage machinery works alongside the registry.
    await this.ctx.storage.put('key', 'value');
    strictEqual(await this.ctx.storage.get('key'), 'value');

    return 'ok';
  }
}

export const durableObjectTest = {
  async test(ctrl, env) {
    const id = env.ns.idFromName('test');
    const stub = env.ns.get(id);
    strictEqual(await stub.check(), 'ok');
  },
};
