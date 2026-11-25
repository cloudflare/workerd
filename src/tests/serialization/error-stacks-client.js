// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { describe, makeError } from 'error-stacks-common';

// Whether each server uses enhanced error serialization. env.ENHANCED says
// whether this client does.
const SERVERS = { SERVER_ENHANCED: true, SERVER_LEGACY: false };

function localMakesError() {
  return makeError('local');
}

// Whichever stack the error has, its first line gives the error's name and
// message.
function check(actual, where, origin, label) {
  const expected = {
    message: `from ${where}`,
    origin,
    firstLineNamesError: true,
  };
  assert.deepStrictEqual(
    actual,
    expected,
    `${label}: ${JSON.stringify(actual)}`
  );
}

// A thrown error never keeps the remote stack.
export const thrown = {
  async test(ctrl, env) {
    for (const server of Object.keys(SERVERS)) {
      const label = `thrown by ${server}`;
      const error = await env[server].throwError().then(
        () => assert.fail(`${label} did not reject`),
        (e) => e
      );
      check(describe(error), 'remote', 'new', label);
    }
  },
};

// A returned error keeps the remote stack only if the remote, which wrote
// it, does not use enhanced error serialization.
export const returned = {
  async test(ctrl, env) {
    for (const [server, serverEnhanced] of Object.entries(SERVERS)) {
      const value = await env[server].returnError();
      check(
        describe(value.deep[1].error),
        'remote',
        serverEnhanced ? 'new' : 'remote',
        `returned by ${server}`
      );
    }
  },
};

// An error sent to the remote keeps the local stack only if this side, which
// wrote it, does not use enhanced error serialization.
export const sent = {
  async test(ctrl, env) {
    for (const server of Object.keys(SERVERS)) {
      const value = { deep: [1, { error: localMakesError() }] };
      check(
        await env[server].describeError(value),
        'local',
        env.ENHANCED ? 'new' : 'local',
        `sent to ${server}`
      );
    }
  },
};

// structuredClone() keeps the stack either way.
export const cloned = {
  test() {
    check(
      describe(structuredClone(localMakesError())),
      'local',
      'local',
      'cloned'
    );
  },
};
