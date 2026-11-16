// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok, strictEqual } from 'assert';

// Caller and callee run with different error serialization flags. Whichever side is enhanced,
// a thrown error must not carry the callee's stack frames into the caller.
function assertNoCalleeStack(env, e, message) {
  strictEqual(e.message, message);
  const stack = String(e.stack ?? '');
  ok(
    !stack.includes(env.CALLEE_MODULE),
    `caller received the callee's stack frames:\n${stack}`
  );
}

export const thrownErrorsDoNotLeakCalleeStack = {
  async test(_, env) {
    try {
      await env.RPC.foo(null);
      throw new Error('Expected error not thrown');
    } catch (e) {
      assertNoCalleeStack(env, e, 'boom');
    }

    try {
      const bar = await env.RPC.bar(null);
      const baz = await bar.baz(null);
      await baz.qux(null);
      throw new Error('Expected error not thrown');
    } catch (e) {
      assertNoCalleeStack(env, e, 'bork');
    }

    try {
      await env.RPC.xyz(null);
      throw new Error('Expected error not thrown');
    } catch (e) {
      assertNoCalleeStack(env, e, 'bang');
    }
  },
};
