// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const events = [];
const entrypointNames = [
  'entry-constructor',
  'entry-block',
  'entry-message',
  'entry-close',
];
const expectedNames = [
  ...entrypointNames,
  'entry-first-before',
  'entry-second-before',
  'entry-second-after',
  'entry-restore-service',
  'entry-restore-service-after',
  'entry-restore-rpc',
  'entry-restore-rpc-after',
];

function findOpen(name) {
  return events.find(
    (e) => e.event.type === 'spanOpen' && e.event.name === name
  );
}

function hasClosed(name) {
  const open = findOpen(name);
  return (
    open &&
    events.some(
      (e) =>
        e.event.type === 'spanClose' &&
        e.invocationId === open.invocationId &&
        e.spanContext.spanId === open.event.spanId
    )
  );
}

export default {
  tailStream(onset) {
    events.push(onset);
    return (event) => {
      events.push(event);
    };
  },
};

export const validate = {
  async test() {
    for (let i = 0; i < 100 && !expectedNames.every(hasClosed); i++) {
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    for (const name of expectedNames) {
      const open = findOpen(name);
      assert.ok(open, name);
      assert.ok(hasClosed(name), `${name} closed`);
      assert.strictEqual(
        events.filter(
          (e) => e.event.type === 'spanOpen' && e.event.name === name
        ).length,
        1,
        name
      );
      const onset = events.find(
        (e) => e.invocationId === open.invocationId && e.event.type === 'onset'
      );
      assert.strictEqual(open.spanContext.traceId, onset.spanContext.traceId);
      assert.strictEqual(open.spanContext.spanId, onset.event.spanId);
      // Logs retain their existing newest-request routing during overlapping actor events.
      if (!entrypointNames.includes(name)) continue;
      const log = events.find(
        (e) =>
          e.invocationId === open.invocationId &&
          e.event.type === 'log' &&
          JSON.stringify(e.event.message).includes(name)
      );
      assert.ok(log, name);
      assert.strictEqual(log.spanContext.traceId, onset.spanContext.traceId);
      assert.strictEqual(log.spanContext.spanId, onset.event.spanId);
      const values =
        typeof log.event.message === 'string'
          ? JSON.parse(log.event.message)
          : log.event.message;
      const identity = JSON.parse(values[0]);
      assert.strictEqual(identity.traceId, onset.spanContext.traceId);
      assert.strictEqual(identity.spanId, onset.event.spanId);
    }

    const first = findOpen('entry-first-before');
    const second = findOpen('entry-second-before');
    assert.notStrictEqual(first.invocationId, second.invocationId);
    // A newer actor event ends the older request's drain and expires its tracer. Its
    // continuation must not submit spans to the newer request as a fallback.
    assert.strictEqual(findOpen('entry-first-after'), undefined);
    assert.strictEqual(
      findOpen('entry-second-after').invocationId,
      second.invocationId
    );
    for (const kind of ['service', 'rpc']) {
      assert.strictEqual(
        findOpen(`entry-restore-${kind}-after`).invocationId,
        findOpen(`entry-restore-${kind}`).invocationId
      );
    }
  },
};
