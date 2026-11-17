// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { describe, makeError } from 'error-stacks-common';

// Whether each server uses enhanced error serialization. env.ENHANCED says
// whether this client does.
const SERVERS = { SERVER_ENHANCED: true, SERVER_LEGACY: false };
const KINDS = ['native', 'cloneable'];

function localMakesError(kind) {
  return makeError(kind, 'local');
}

// A native error and a CloneableTestError get the same stack in each case.
// Either way, the stack's first line gives the error's name and message, and
// a CloneableTestError's stack is not enumerable. (A native error's is when
// structuredClone() restores it with enhanced error serialization.)
function check(
  actual,
  kind,
  where,
  origin,
  cloneable,
  label,
  message = `${kind} from ${where}`
) {
  const expected = {
    cloneable,
    message,
    origin,
    firstLineNamesError: true,
    enumerable: kind === 'native' ? actual.enumerable : false,
  };
  assert.deepStrictEqual(
    actual,
    expected,
    `${label}: ${JSON.stringify(actual)}`
  );
}

// A thrown error never keeps the remote stack. It reaches the caller as
// itself only if both sides use enhanced error serialization; otherwise the
// caller makes a new Error from its description, whose message, as for any
// error whose name is not a built-in one, begins with its name.
export const thrown = {
  async test(ctrl, env) {
    for (const [server, serverEnhanced] of Object.entries(SERVERS)) {
      for (const kind of KINDS) {
        const label = `${kind} thrown by ${server}`;
        const error = await env[server].throwError(kind).then(
          () => assert.fail(`${label} did not reject`),
          (e) => e
        );
        const asItself = env.ENHANCED && serverEnhanced;
        const renamed = kind === 'cloneable' && !asItself;
        check(
          describe(error),
          kind,
          'remote',
          'new',
          kind === 'cloneable' && asItself,
          label,
          renamed ? 'CloneableTestError: cloneable from remote' : undefined
        );
      }
    }
  },
};

// A returned error keeps the remote stack only if the remote, which wrote
// it, does not use enhanced error serialization.
export const returned = {
  async test(ctrl, env) {
    for (const [server, serverEnhanced] of Object.entries(SERVERS)) {
      for (const kind of KINDS) {
        const value = await env[server].returnError(kind);
        check(
          describe(value.deep[1].error),
          kind,
          'remote',
          serverEnhanced ? 'new' : 'remote',
          kind === 'cloneable',
          `${kind} returned by ${server}`
        );
      }
    }
  },
};

// An error sent to the remote keeps the local stack only if this side, which
// wrote it, does not use enhanced error serialization.
export const sent = {
  async test(ctrl, env) {
    for (const server of Object.keys(SERVERS)) {
      for (const kind of KINDS) {
        const value = { deep: [1, { error: localMakesError(kind) }] };
        check(
          await env[server].describeError(value),
          kind,
          'local',
          env.ENHANCED ? 'new' : 'local',
          kind === 'cloneable',
          `${kind} sent to ${server}`
        );
      }
    }
  },
};

// structuredClone() keeps the stack either way.
export const cloned = {
  test(ctrl, env) {
    for (const kind of KINDS) {
      check(
        describe(structuredClone(localMakesError(kind))),
        kind,
        'local',
        'local',
        kind === 'cloneable',
        `${kind} cloned`
      );
    }
  },
};
