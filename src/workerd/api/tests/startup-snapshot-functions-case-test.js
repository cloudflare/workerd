// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-functions-test.js: MODE names the function the top level keeps.

import { AsyncLocalStorage } from 'node:async_hooks';
import { createRequire } from 'node:module';
import { env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

// createRequire() returns a v8::Function over a static callback whose data is the referrer, a
// string, which a startup snapshot serializes as it is.
const require = MODE === 'create-require' ? createRequire('/worker.js') : null;

// AsyncLocalStorage.bind() at the top level, in the root frame, returns a bound function with no
// C++ state: the wrapped function and receiver ride as bound arguments of a static callback.
// (Bound inside a frame, it would also carry the frame's opaque wrapper, which has no transfer.)
const bound = MODE === 'bind' ? AsyncLocalStorage.bind(() => 'bound') : null;

// The runtime binds with the `bind` the context was created with, which a Worker started from the
// snapshot still has, whatever the top level did to Function.prototype.
let replacedBindCalls = 0;
if (MODE === 'bind') {
  const originalBind = Function.prototype.bind;
  Function.prototype.bind = function (...args) {
    replacedBindCalls++;
    return Reflect.apply(originalBind, this, args);
  };
}

function checks() {
  switch (MODE) {
    case 'create-require': {
      const lib = require('./lib.cjs');
      return {
        required: typeof lib.token === 'object',
        cached: require('./lib.cjs') === lib,
      };
    }
    case 'bind': {
      const callsBefore = replacedBindCalls;
      const boundAgain = AsyncLocalStorage.bind(() => 'again');
      return {
        called: bound() === 'bound',
        boundAgain: boundAgain() === 'again',
        replacedBindNotCalled: replacedBindCalls === callsBefore,
      };
    }
  }
  return {};
}

export default {
  async fetch() {
    return Response.json({
      mode: MODE,
      snapshotUsed: unsafe.startedFromStartupSnapshot(),
      checks: checks(),
    });
  },
};
