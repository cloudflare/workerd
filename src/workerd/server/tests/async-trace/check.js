// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Checks the --async-trace output of scenario.js: trace.ndjson (with --async-trace-stacks) and
// trace-promises.ndjson (with --async-trace-promises).

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
    if (event.e === 'exit') continue;
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

async function load(env, name) {
  const response = await env.TRACE_DIR.fetch(`http://trace-dir/${name}`);
  assert.strictEqual(response.status, 200, name);
  return parse(await response.text());
}

// Checks what holds for any trace of the scenario: the header, each context's framing and stats,
// balanced callback scopes, time order, and the resources of the core (non-promise) tier.
function checkCore(events, { withStacks }) {
  {
    const [header] = events;
    assert.strictEqual(header.e, 'header');
    assert.strictEqual(header.v, 1);
    assert.strictEqual(header.producer, 'workerd');
  }
  {
    // workerd exits without closing contexts, so the trace ends with `exit`, listing those still
    // open: none, here.
    const exits = events.filter((e) => e.e === 'exit');
    assert.strictEqual(exits.length, 1, 'one exit line');
    assert.strictEqual(events[events.length - 1], exits[0], 'exit is last');
    assert.deepStrictEqual(exits[0].open, [], 'every context ended');
  }
  const indexed = index(events);
  const { contexts, resources, stacks } = indexed;
  assert.strictEqual(
    contexts.size,
    3,
    'the test, its subrequest, and its queue message'
  );

  for (const [ctx, list] of contexts) {
    assert.strictEqual(list[0].e, 'ctx', `context ${ctx} starts with ctx`);
    const end = list[list.length - 1];
    assert.strictEqual(end.e, 'ctx_end', `context ${ctx} ends with ctx_end`);
    for (const stat of [
      'dropped',
      'unknown',
      'unbalanced',
      'ambiguousBindings',
      'unusedOperationNames',
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
  if (withStacks) {
    for (const resource of [timer, microtask, fetch]) {
      assert(stackFunctions(stacks, resource).includes('test'), resource.name);
    }
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

  // An internal stream's I/O is named: the read and the write are operations, each resuming the
  // handler.
  for (const name of ['stream_read', 'stream_write']) {
    const operation = find(resources, byKind('operation', name), name);
    assert.strictEqual(operation.ctx, test.ctx, name);
    assert(turnCauses.has(operation.id), `${name} resumes the handler`);
  }

  // The subrequest: scheduler.wait, bridged back to JavaScript by awaitIo.
  const wait = find(
    resources,
    byKind('timer', 'scheduler.wait'),
    'scheduler.wait'
  );
  assert.notStrictEqual(wait.ctx, test.ctx);
  const subrequest = find(
    resources,
    (r) => byKind('request', 'fetch')(r) && r.ctx === wait.ctx,
    'subrequest'
  );

  // The queue send's span is lent to the awaitIo waiting for it, so the send resumes the handler.
  const queueSend = find(
    resources,
    byKind('operation', 'queue_send'),
    'queue_send'
  );
  assert.strictEqual(queueSend.ctx, test.ctx);
  assert(turnCauses.has(queueSend.id), 'the queue send resumes the handler');
  const message = find(
    resources,
    (r) =>
      byKind('request', 'fetch')(r) && r.ctx !== wait.ctx && r.ctx !== test.ctx,
    'queue message'
  );

  // The service binding delivers the subrequest synchronously, so it links to the caller's fetch;
  // likewise the queue message, to the queue send.
  const callerIso = events.find((e) => e.e === 'ctx' && e.ctx === test.ctx).iso;
  const links = events
    .filter((e) => e.e === 'link')
    .map(({ ctx, id, fromIso, fromCtx, fromId }) => ({
      ctx,
      id,
      fromIso,
      fromCtx,
      fromId,
    }));
  assert.deepStrictEqual(links, [
    {
      ctx: subrequest.ctx,
      id: subrequest.id,
      fromIso: callerIso,
      fromCtx: test.ctx,
      fromId: fetch.id,
    },
    {
      ctx: message.ctx,
      id: message.id,
      fromIso: callerIso,
      fromCtx: test.ctx,
      fromId: queueSend.id,
    },
  ]);
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
  if (withStacks) {
    assert(
      stackFunctions(stacks, wait).includes('fetch'),
      'scheduler.wait stack'
    );
  }
  return { ...indexed, test, timer, microtask };
}

// With --async-trace-promises: the code after `await new Promise((r) => setTimeout(r, 1))` runs as
// a promise reaction, linked to the awaited promise, which the timer's callback settles.
function checkPromises({ contexts, resources, test, timer, microtask }) {
  const promises = [...resources.values()].filter(
    (r) => r.kind === 'js_promise'
  );
  assert(promises.length > 0, 'promises are traced');
  for (const promise of promises) {
    assert(
      resources.has(promise.trigger),
      `promise ${promise.id} has a known trigger`
    );
  }

  const reaction = resources.get(microtask.exec);
  assert.strictEqual(
    reaction?.kind,
    'js_promise',
    'the continuation runs as a reaction'
  );
  const awaited = resources.get(reaction.trigger);
  assert.strictEqual(
    awaited?.kind,
    'js_promise',
    'the reaction derives from a promise'
  );
  assert.strictEqual(
    awaited.trigger,
    test.id,
    'the handler created the awaited promise'
  );

  const list = contexts.get(test.ctx);
  const index = (predicate, what) => {
    const i = list.findIndex(predicate);
    assert(i >= 0, what);
    return i;
  };
  const timerBefore = index(
    (e) => e.e === 'before' && e.id === timer.id,
    'timer runs'
  );
  const timerAfter = index(
    (e) => e.e === 'after' && e.id === timer.id,
    'timer finishes'
  );
  const settled = index(
    (e) => e.e === 'settle' && e.id === awaited.id,
    'awaited settles'
  );
  assert(
    timerBefore < settled && settled < timerAfter,
    'the timer settles the awaited promise'
  );
}

export default {
  async test(ctrl, env) {
    const core = checkCore(await load(env, 'trace.ndjson'), {
      withStacks: true,
    });
    assert(
      ![...core.resources.values()].some((r) => r.kind === 'js_promise'),
      'promises are traced only with --async-trace-promises'
    );

    const withPromises = checkCore(await load(env, 'trace-promises.ndjson'), {
      withStacks: false,
    });
    checkPromises(withPromises);
  },
};
