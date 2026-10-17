// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-functions-test.js: MODE names the function the top level keeps.

import { AsyncLocalStorage } from 'node:async_hooks';
import { createRequire } from 'node:module';
import { env } from 'cloudflare:workers';

const MODE = env.MODE;

// Whether this Worker was started from a startup snapshot rather than running its top level
// itself. In the zygote's env a service binding is an `undefined` placeholder (the started Worker
// supplies the value: isDeferredForSnapshot in server/workerd-api.c++), so at module scope SELF
// is undefined exactly when this code runs in the zygote, and the constant is baked into the
// snapshot.
const snapshotUsed = typeof env.SELF === 'undefined';

// createRequire() returns a jsg::Function: a v8::Function created in C++ whose native state, the
// referrer, can be copied into a Worker started from the snapshot.
const require = MODE === 'create-require' ? createRequire('/worker.js') : null;

// AsyncLocalStorage.bind() returns a jsg::Function too, but its native state holds the bound
// function and receiver, which cannot be copied.
const bound = MODE === 'bind' ? AsyncLocalStorage.bind(() => 'bound') : null;

function checks() {
  switch (MODE) {
    case 'create-require': {
      const lib = require('./lib.cjs');
      return {
        required: typeof lib.token === 'object',
        cached: require('./lib.cjs') === lib,
      };
    }
    case 'bind':
      return { called: bound() === 'bound' };
  }
  return {};
}

export default {
  async fetch() {
    return Response.json({ mode: MODE, snapshotUsed, checks: checks() });
  },
};
