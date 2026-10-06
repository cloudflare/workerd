// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

const invocations = [];
const HUNG_REQUEST_MESSAGE =
  "The Workers runtime canceled this request because it detected that your Worker's code had hung and would never generate a response. Refer to: https://developers.cloudflare.com/workers/observability/errors/";

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

function findInvocation(method) {
  return invocations.find((invocation) => invocation.methods.includes(method));
}

function hasException(invocation, message) {
  return invocation?.exceptions.some(
    (exception) => exception.message === message
  );
}

function foundExpectedEvents(throwing, asyncThrowing) {
  return (
    hasException(throwing, 'intentional JSRPC failure') &&
    throwing?.outcomes.includes('exception') &&
    hasException(asyncThrowing, 'intentional async JSRPC failure') &&
    asyncThrowing?.outcomes.includes('exception')
  );
}

export const test = {
  async test() {
    const deadline = Date.now() + 5000;
    let throwing = findInvocation('throwError');
    let asyncThrowing = findInvocation('throwAsyncError');

    while (
      !foundExpectedEvents(throwing, asyncThrowing) &&
      Date.now() < deadline
    ) {
      await scheduler.wait(10);
      throwing = findInvocation('throwError');
      asyncThrowing = findInvocation('throwAsyncError');
    }

    assert.ok(throwing, 'Could not find throwError JSRPC invocation');
    assert.ok(asyncThrowing, 'Could not find throwAsyncError JSRPC invocation');
    assert.strictEqual(
      invocations.length,
      2,
      'Expected exactly two callee invocations'
    );

    assert.deepStrictEqual(throwing.methods, ['throwError']);
    assert.deepStrictEqual(throwing.exceptions, [
      {
        name: 'Error',
        message: 'intentional JSRPC failure',
      },
      {
        name: 'Error',
        message: HUNG_REQUEST_MESSAGE,
      },
    ]);
    assert.deepStrictEqual(throwing.outcomes, ['exception']);

    assert.deepStrictEqual(asyncThrowing.methods, ['throwAsyncError']);
    assert.deepStrictEqual(asyncThrowing.exceptions, [
      {
        name: 'Error',
        message: 'intentional async JSRPC failure',
      },
    ]);
    assert.deepStrictEqual(asyncThrowing.outcomes, ['exception']);
  },
};
