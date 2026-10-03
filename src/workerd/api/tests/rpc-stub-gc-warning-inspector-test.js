// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

const WARNING_PREFIXES = [
  'An RPC stub was not disposed properly',
  'An RPC result was not disposed properly',
];
const TIMEOUT_MS = 5000;
const POLL_MS = 10;

let fetchOutcomes = 0;
const warnings = [];

export default {
  tailStream(onset) {
    const isFetch = onset.event.info?.type === 'fetch';
    return (event) => {
      if (
        event.event.type === 'log' &&
        event.event.level === 'warn' &&
        WARNING_PREFIXES.some((prefix) =>
          event.event.message?.[0]?.startsWith(prefix)
        )
      ) {
        warnings.push(event.event.message[0]);
      }
      if (isFetch && event.event.type === 'outcome') {
        fetchOutcomes++;
      }
    };
  },
};

async function waitForFetchOutcome(count) {
  const deadline = Date.now() + TIMEOUT_MS;
  while (fetchOutcomes < count && Date.now() < deadline) {
    await scheduler.wait(POLL_MS);
  }
  assert.strictEqual(
    fetchOutcomes,
    count,
    `fetch outcome ${count} was not tailed`
  );
}

export const test = {
  async test(ctrl, env) {
    let response = await env.TRIGGER.fetch('http://example.com/?dispose');
    assert.strictEqual(await response.text(), 'disposed');
    await waitForFetchOutcome(1);
    assert.deepStrictEqual(warnings, []);

    response = await env.TRIGGER.fetch('http://example.com/');
    assert.match(await response.text(), /^leaked: /);
    await waitForFetchOutcome(2);
    assert.strictEqual(warnings.length, 1);
  },
};
