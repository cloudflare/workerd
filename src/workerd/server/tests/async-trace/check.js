// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Checks the --async-trace output of scenario.js.

import assert from 'node:assert';

function parse(text) {
  assert(text.endsWith('\n'), 'the file ends with a complete line');
  return text
    .slice(0, -1)
    .split('\n')
    .map((line) => JSON.parse(line));
}

// Groups events by context. Resource IDs are per isolate, so one map serves all contexts here.
function index(events) {
  const contexts = new Map();
  const resources = new Map();
  for (const event of events.slice(1)) {
    if (!contexts.has(event.ctx)) contexts.set(event.ctx, []);
    contexts.get(event.ctx).push(event);
    if (event.e === 'init') resources.set(event.id, event);
  }
  return { contexts, resources };
}

function find(resources, predicate, what) {
  const found = [...resources.values()].filter(predicate);
  assert.strictEqual(found.length, 1, `exactly one ${what}`);
  return found[0];
}

export default {
  async test(ctrl, env) {
    const response = await env.TRACE_DIR.fetch('http://trace-dir/trace.ndjson');
    assert.strictEqual(response.status, 200);
    const events = parse(await response.text());

    const [header] = events;
    assert.strictEqual(header.e, 'header');
    assert.strictEqual(header.v, 1);
    assert.strictEqual(header.producer, 'workerd');

    const { contexts, resources } = index(events);
    assert.strictEqual(contexts.size, 2, 'the test and its subrequest');

    for (const [ctx, list] of contexts) {
      assert.strictEqual(list[0].e, 'ctx', `context ${ctx} starts with ctx`);
      const end = list[list.length - 1];
      assert.strictEqual(end.e, 'ctx_end', `context ${ctx} ends with ctx_end`);
      for (const stat of [
        'dropped',
        'unknown',
        'unbalanced',
        'ambiguousBindings',
        'foreignThread',
      ]) {
        assert.strictEqual(end[stat], 0, `context ${ctx}: ${stat}`);
      }

      // Callback scopes balance, and times within a context never go backwards.
      const open = [];
      let last = 0;
      for (const event of list) {
        if (event.e === 'before') open.push(event.id);
        if (event.e === 'after') assert.strictEqual(open.pop(), event.id);
        const at = event.at ?? event.end;
        if (at !== undefined) {
          assert(
            at >= last,
            `context ${ctx}: time order at ${JSON.stringify(event)}`
          );
          last = at;
        }
      }
      assert.strictEqual(open.length, 0, `context ${ctx}: no scope left open`);
    }

    const turnCauses = new Set(
      events.filter((e) => e.e === 'turn').map((e) => e.cause)
    );
    const byKind = (kind, name) => (r) => r.kind === kind && r.name === name;

    // The test handler: setTimeout, then a fetch created in the timer's turn.
    const test = find(resources, byKind('request', 'test'), 'test request');
    const timer = find(resources, byKind('timer', 'setTimeout'), 'setTimeout');
    assert.strictEqual(timer.trigger, test.id);
    assert(turnCauses.has(timer.id), 'the timer causes a turn');
    const fetch = find(
      resources,
      byKind('operation', 'fetch'),
      'fetch operation'
    );
    assert.strictEqual(
      fetch.trigger,
      timer.id,
      'fetch is triggered by the timer'
    );
    const microtask = find(
      resources,
      byKind('microtask', 'queueMicrotask'),
      'microtask'
    );
    assert.strictEqual(microtask.trigger, timer.id);

    // The fetch's response resumes JavaScript under the fetch operation (adopted by awaitIo), and
    // reading the body is triggered by it.
    assert(turnCauses.has(fetch.id), 'the fetch operation causes a turn');
    const bridges = [...resources.values()].filter(
      (r) => r.ctx === test.ctx && r.kind === 'kj_to_js'
    );
    assert(
      bridges.some((r) => r.trigger === fetch.id),
      'reading the body is triggered by the fetch'
    );

    // The subrequest: scheduler.wait, bridged back to JavaScript by awaitIo.
    const wait = find(
      resources,
      byKind('timer', 'scheduler.wait'),
      'scheduler.wait'
    );
    assert.notStrictEqual(wait.ctx, test.ctx);
    const waitBridge = [...resources.values()].find(
      (r) => r.ctx === wait.ctx && r.kind === 'kj_to_js'
    );
    assert(waitBridge, 'scheduler.wait is awaited through awaitIo');
    assert.strictEqual(
      waitBridge.trigger,
      wait.trigger,
      'both created by the handler'
    );
    assert(turnCauses.has(waitBridge.id), 'the bridge resumes the handler');
  },
};
