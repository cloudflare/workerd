// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';

const invocations = new Map();
const owners = new Map();
const logs = [];
const diagnostics = [];
const requestIds = new Map();
const outcomes = new Map();
function outcome(id) {
  if (!outcomes.has(id)) outcomes.set(id, Promise.withResolvers());
  return outcomes.get(id);
}

export default {
  async fetch(request) {
    await outcome(new URL(request.url).pathname.slice(1)).promise;
    return new Response('ended');
  },
  tailStream(onset) {
    const stream = onset.invocationId;
    const ended = Promise.withResolvers();
    invocations.set(stream, ended.promise);
    owners.set(onset.event.spanId, {
      traceId: onset.spanContext.traceId,
      invocationId: stream,
    });
    return (event) => {
      if (event.event.type === 'spanOpen') {
        assert(!owners.has(event.event.spanId), 'unique span owner');
        owners.set(event.event.spanId, {
          traceId: event.spanContext.traceId,
          invocationId: event.invocationId,
        });
      } else if (event.event.type === 'log') {
        const body = JSON.parse(event.event.message[0]);
        if (body.phase === 'start') requestIds.set(stream, body.id);
        logs.push({
          ...event,
          stream,
          body,
        });
      } else if (event.event.type === 'diagnosticChannel') {
        diagnostics.push({ ...event, stream });
      } else if (event.event.type === 'outcome') {
        ended.resolve();
        const id = requestIds.get(stream);
        if (id !== undefined) outcome(id).resolve();
      }
    };
  },
};

export const validate = {
  async test() {
    let count;
    do {
      count = invocations.size;
      await Promise.all(invocations.values());
    } while (invocations.size > count);
    assert.equal(logs.length, 46, 'all foreground and background logs');
    const starts = new Map(
      logs.filter((e) => e.body.phase === 'start').map((e) => [e.body.id, e])
    );
    assert.equal(starts.size, 10);
    for (const log of logs) {
      // Once A's trace has ended, B owns the object's continuing background work.
      const id =
        log.body.id === 'background/a' && log.body.phase !== 'start'
          ? 'background/b'
          : log.body.id;
      const start = starts.get(id);
      const owner = owners.get(log.spanContext.spanId);
      const label = `${log.body.id}: ${log.body.phase}`;
      assert(owner, `${label}: span owner exists`);
      assert.equal(
        log.spanContext.traceId,
        owner.traceId,
        `${label}: native trace/span pair`
      );
      assert.equal(
        log.invocationId,
        owner.invocationId,
        `${label}: span's invocation`
      );
      assert.equal(
        log.invocationId,
        start.invocationId,
        `${label}: originating invocation`
      );
      assert.equal(
        log.stream,
        start.stream,
        `${label}: originating tail stream`
      );
      assert.equal(
        log.spanContext.traceId,
        start.spanContext.traceId,
        `${label}: originating trace`
      );
      if (log.body.phase === 'nested') {
        assert.notEqual(log.spanContext.spanId, start.spanContext.spanId);
      } else {
        assert.equal(
          log.spanContext.spanId,
          start.spanContext.spanId,
          `${label}: restored span`
        );
      }
    }
    assert.equal(diagnostics.length, 9);
    for (const event of diagnostics) {
      const owner = owners.get(event.spanContext.spanId);
      assert(owner);
      assert.equal(event.spanContext.traceId, owner.traceId);
      assert.equal(event.invocationId, owner.invocationId);
      assert.equal(event.stream, owner.invocationId);
    }
  },
};
