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

// createRequire() returns a jsg::Function: a v8::Function created in C++ whose native state, the
// referrer, can be copied into a Worker started from the snapshot.
const require = MODE === 'create-require' ? createRequire('/worker.js') : null;

// AsyncLocalStorage.bind() returns a jsg::Function too, but its native state holds the bound
// function and receiver, which cannot be copied.
const bound = MODE === 'bind' ? AsyncLocalStorage.bind(() => 'bound') : null;

// The globals atob() and btoa(), which the top level keeps, calls, and makes throw. They are C++
// methods of the global scope, or under the TS_ATOB_BTOA autogate (in the @all-autogates variant)
// functions of the per-isolate bootstrap that call into C++.
function caught(fn, input) {
  try {
    fn(input);
  } catch (error) {
    return error;
  }
  return undefined;
}
const base64 =
  MODE === 'atob-btoa'
    ? {
        atob,
        btoa,
        encoded: btoa('snapshot\xff'),
        decoded: atob('c25hcHNob3T/'),
        error: caught(atob, '!'),
      }
    : null;

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
    case 'atob-btoa': {
      // Called without a receiver: the functions accept only the global as one.
      const { atob: keptAtob, btoa: keptBtoa } = base64;
      const error = caught(keptBtoa, '\u0100');
      return {
        sameFunctions: keptAtob === atob && keptBtoa === btoa,
        topLevelResults:
          base64.encoded === 'c25hcHNob3T/' &&
          base64.decoded === 'snapshot\xff',
        decodes: keptAtob(' c25h cHNob3T/ ') === 'snapshot\xff',
        encodes: keptBtoa('snapshot\xff') === 'c25hcHNob3T/',
        topLevelError:
          base64.error instanceof DOMException &&
          base64.error.name === 'InvalidCharacterError',
        throws:
          error instanceof DOMException &&
          error.name === 'InvalidCharacterError' &&
          error.code === 5,
        checksReceiver: caught(keptAtob.bind({}), '') instanceof TypeError,
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
