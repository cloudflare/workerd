// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';

export const terminalSettlementPreservesLifecycle = {
  async test(_controller, env) {
    for (const kind of ['values', 'bytes', 'byob']) {
      for (const operation of ['close', 'error']) {
        const query = new URLSearchParams({ kind, operation });
        const origin = await env.LIFECYCLE.fetch(
          `https://lifecycle/origin?${query}`
        );
        strictEqual(await origin.text(), 'origin complete');
        const response = await env.LIFECYCLE.fetch(
          `https://lifecycle/settle?${query}`
        );
        const terminal =
          operation === 'close' ? { done: true } : { error: 'source error' };
        deepStrictEqual(await response.json(), {
          entries: 1,
          desiredSize: operation === 'close' ? 0 : null,
          nestedError: operation === 'close' ? null : 'TypeError',
          outerReturned: true,
          current: [terminal, terminal, terminal],
          subsequent: [terminal, terminal, terminal],
          fresh: [terminal, terminal, terminal],
          closed:
            operation === 'close'
              ? ['closed', 'closed', 'closed']
              : ['source error', 'source error', 'source error'],
        });
      }
    }
  },
};

export const closeDrainsBufferedData = {
  async test() {
    for (const kind of ['values', 'bytes', 'byob']) {
      let controller;
      const source = {
        start(value) {
          controller = value;
        },
      };
      if (kind !== 'values') source.type = 'bytes';
      const stream = new ReadableStream(source);
      controller.enqueue(
        kind === 'values' ? 'buffered' : new Uint8Array([17, 34, 51])
      );
      controller.close();
      const reader = stream.getReader(
        kind === 'byob' ? { mode: 'byob' } : undefined
      );
      const read = () =>
        kind === 'byob' ? reader.read(new Uint8Array(8)) : reader.read();
      const result = await read();
      strictEqual(result.done, false);
      deepStrictEqual(
        kind === 'values' ? result.value : Array.from(result.value),
        kind === 'values' ? 'buffered' : [17, 34, 51]
      );
      strictEqual((await read()).done, true);
      await reader.closed;
    }
  },
};

export const cachedByobResponseDuringClose = {
  async test(_controller, env) {
    for (const pedantic of [false, true]) {
      const service = pedantic ? env.PEDANTIC_LIFECYCLE : env.LIFECYCLE;
      for (const method of ['respond', 'new-view']) {
        const query = new URLSearchParams({ cached: 'true', pedantic, method });
        const origin = await service.fetch(`https://lifecycle/origin?${query}`);
        strictEqual(await origin.text(), 'origin complete');
        const response = await service.fetch(
          `https://lifecycle/settle?${query}`
        );
        deepStrictEqual(await response.json(), {
          entries: 1,
          desiredSize: 0,
          viewBefore: 8,
          validationError:
            pedantic && method === 'new-view' ? 'RangeError' : null,
          responded: true,
          viewCleared: true,
          cachedCleared: true,
          current: { done: true, length: 0 },
          subsequent: { done: true, length: 0 },
          fresh: [{ done: true }, { done: true }],
          closed: ['closed', 'closed'],
        });
      }
    }
  },
};

async function checkBufferedTerminal(env, operation) {
  const query = new URLSearchParams({
    kind: 'byob',
    buffered: 'true',
    operation,
  });
  const origin = await env.LIFECYCLE.fetch(`https://lifecycle/origin?${query}`);
  strictEqual(await origin.text(), 'origin complete');
  const response = await env.LIFECYCLE.fetch(
    `https://lifecycle/settle?${query}`
  );
  const terminal =
    operation === 'close'
      ? { done: true, bytes: [] }
      : { error: 'source error' };
  deepStrictEqual(await response.json(), {
    entries: 1,
    desiredSize: operation === 'close' ? 0 : null,
    nestedError: operation === 'close' ? null : 'TypeError',
    outerReturned: true,
    prefixPending: true,
    current: [
      terminal,
      { done: false, bytes: [17] },
      { done: false, bytes: [17] },
    ],
    subsequent: [terminal, terminal, terminal],
    fresh: [terminal, terminal, terminal],
    closed:
      operation === 'close'
        ? ['closed', 'closed', 'closed']
        : ['source error', 'source error', 'source error'],
  });
}

export const bufferedClosePreservesLifecycle = {
  async test(_controller, env) {
    await checkBufferedTerminal(env, 'close');
  },
};

export const bufferedErrorPreservesLifecycle = {
  async test(_controller, env) {
    await checkBufferedTerminal(env, 'error');
  },
};
