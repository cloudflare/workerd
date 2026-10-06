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

function findInvocation(method) {
  return invocations.find((invocation) => invocation.methods.includes(method));
}

function invocationsFor(method) {
  return invocations.filter((invocation) => invocation.methods.includes(method));
}

function foundExpectedEvents(throwing, asyncThrowing, pending) {
  return (
    throwing?.exceptions.some(
      (exception) => exception.message === 'intentional JSRPC failure'
    ) &&
    throwing?.outcomes.includes('exception') &&
    asyncThrowing?.exceptions.some(
      (exception) => exception.message === 'intentional async JSRPC failure'
    ) &&
    asyncThrowing?.outcomes.includes('exception') &&
    pending?.outcomes.includes('ok')
  );
}

export const test = {
  async test() {
    const deadline = Date.now() + 5000;
    let throwing = findInvocation('throwError');
    let asyncThrowing = findInvocation('throwAsyncError');
    let pending = findInvocation('neverResolves');

    while (
      !foundExpectedEvents(throwing, asyncThrowing, pending) &&
      Date.now() < deadline
    ) {
      await scheduler.wait(10);
      throwing = findInvocation('throwError');
      asyncThrowing = findInvocation('throwAsyncError');
      pending = findInvocation('neverResolves');
    }

    assert.ok(throwing, 'Could not find throwError JSRPC invocation');
    assert.ok(asyncThrowing, 'Could not find throwAsyncError JSRPC invocation');
    assert.ok(pending, 'Could not find neverResolves JSRPC invocation');
    assert.strictEqual(invocations.length, 3, 'Expected exactly three callee invocations');
    assert.strictEqual(invocationsFor('throwError').length, 1);
    assert.strictEqual(invocationsFor('throwAsyncError').length, 1);
    assert.strictEqual(invocationsFor('neverResolves').length, 1);

    assert.deepStrictEqual(throwing.methods, ['throwError']);
    assert.deepStrictEqual(throwing.exceptions, [
      {
        name: 'Error',
        message: 'intentional JSRPC failure',
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

    assert.deepStrictEqual(pending.methods, ['neverResolves']);
    assert.deepStrictEqual(pending.logs, [['callee neverResolves called']]);
    assert.deepStrictEqual(pending.exceptions, []);
    assert.deepStrictEqual(pending.outcomes, ['ok']);
  },
};
