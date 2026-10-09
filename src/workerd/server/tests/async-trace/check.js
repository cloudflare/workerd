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

// Groups events by context. Resource and stack IDs are per isolate, and there is one isolate here,
// so one map of each serves all contexts.
function index(events) {
  const contexts = new Map();
  const resources = new Map();
  const stacks = new Map();
  for (const event of events.slice(1)) {
    if (event.e === 'stack') {
      stacks.set(event.id, event.frames);
      continue;
    }
    if (!contexts.has(event.ctx)) contexts.set(event.ctx, []);
    contexts.get(event.ctx).push(event);
    if (event.e === 'init') {
      // A stack is written before the first event that refers to it.
      if (event.stack !== undefined) {
        assert(
          stacks.has(event.stack),
          `stack ${event.stack} precedes ${JSON.stringify(event)}`
        );
      }
      resources.set(event.id, event);
    }
  }
  return { contexts, resources, stacks };
}

// The function names in a resource's creation stack (run with --async-trace-stacks).
function stackFunctions(stacks, resource) {
  assert(resource.stack !== undefined, `${resource.name} has a creation stack`);
  return stacks.get(resource.stack).map((frame) => frame.fn);
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

    const { contexts, resources, stacks } = index(events);
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

    // Creation stacks point at the code that created each resource.
    for (const resource of [timer, microtask, fetch]) {
      assert(stackFunctions(stacks, resource).includes('test'), resource.name);
    }

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
    assert(
      stackFunctions(stacks, wait).includes('fetch'),
      'scheduler.wait stack'
    );
  },
};
