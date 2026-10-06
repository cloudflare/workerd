// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

const invocations = [];

export default {
  tailStream(onset) {
    if (
      onset.event.info?.type !== 'jsrpc' ||
      onset.event.entrypoint !== 'ThrowingService'
    ) {
      return () => {};
    }

    const invocation = {
      methods: [],
      logs: [],
      exceptions: [],
      outcomes: [],
    };
    invocations.push(invocation);

    return (event) => {
      const item = event.event;
      if (item.type === 'attributes') {
        const method = item.info.find(({ name }) => name === 'jsrpc.method');
        const targetKind = item.info.find(
          ({ name }) => name === 'jsrpc.target_kind'
        );
        const operation = item.info.find(
          ({ name }) => name === 'jsrpc.operation'
        );
        if (
          method?.value &&
          targetKind?.value === 'entrypoint' &&
          operation?.value === 'call'
        ) {
          invocation.methods.push(method.value);
        }
      } else if (item.type === 'log') {
        invocation.logs.push(item.message);
      } else if (item.type === 'exception') {
        invocation.exceptions.push({ name: item.name, message: item.message });
      } else if (item.type === 'outcome') {
        invocation.outcomes.push(item.outcome);
      }
    };
  },
};

function foundExpectedEvents(invocation) {
  return (
    invocation?.exceptions.some(
      (exception) => exception.message === 'intentional JSRPC failure'
    ) &&
    invocation?.exceptions.some(
      (exception) => exception.message === 'intentional async JSRPC failure'
    ) &&
    invocation?.outcomes.includes('exception')
  );
}

export const test = {
  async test() {
    const deadline = Date.now() + 5000;
    let invocation = invocations[0];

    while (!foundExpectedEvents(invocation) && Date.now() < deadline) {
      await scheduler.wait(10);
      invocation = invocations[0];
    }

    assert.ok(invocation, 'Could not find ThrowingService JSRPC invocation');
    assert.strictEqual(
      invocations.length,
      1,
      'Expected exactly one callee invocation'
    );
    assert.deepStrictEqual(invocation.methods, [
      'throwError',
      'throwAsyncError',
    ]);
    assert.deepStrictEqual(invocation.exceptions, [
      {
        name: 'Error',
        message: 'intentional JSRPC failure',
      },
      {
        name: 'Error',
        message: 'intentional async JSRPC failure',
      },
    ]);
    assert.deepStrictEqual(invocation.outcomes, ['exception']);
  },
};
