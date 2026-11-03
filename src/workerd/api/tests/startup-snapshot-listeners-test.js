// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Top-level addEventListener() on the global scope across a startup snapshot. A case Worker's
// top level registers listeners; at request time it fires the matching events and reports which
// listeners ran, in which order. Every listener appends to the module-level `calls` array, so a
// report also shows that the restored listeners still share the baked module state. The runner
// asserts the reports. Under the startup-snapshot autogate every service here goes through the
// zygote; without it they are plain Workers and the same assertions hold with snapshotUsed false.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

const MODE = env.MODE;

// Whether this Worker was started from a startup snapshot rather than running its top level
// itself. In the zygote's env a service binding is an `undefined` placeholder (the started Worker
// supplies the value: isDeferredForSnapshot in server/workerd-api.c++), so at module scope SELF
// is undefined exactly when this code runs in the zygote, and the constant is baked into the
// snapshot.
const snapshotUsed = typeof env.SELF === 'undefined';

const calls = [];

// Settled by the rejection listeners; created per request by the `rejections` case.
let onUnhandled = null;
let onHandled = null;

// The identity passed to removeEventListener() at request time must be the object the top level
// registered, so it is kept at module scope.
const first = () => calls.push('first');

switch (MODE) {
  case 'runner':
  case 'control':
    break;
  case 'error':
    addEventListener('error', (event) => {
      calls.push(event.message);
      event.preventDefault();
    });
    break;
  case 'rejections':
    addEventListener('unhandledrejection', (event) => {
      event.preventDefault();
      calls.push(`unhandled:${event.reason.message}`);
      onUnhandled?.();
    });
    addEventListener('rejectionhandled', () => {
      calls.push('handled');
      onHandled?.();
    });
    break;
  case 'listeners':
    addEventListener('ping', first);
    addEventListener('ping', {
      handleEvent() {
        calls.push('second');
      },
    });
    addEventListener('ping', () => calls.push('third'), { once: true });
    // A once-listener the top level already consumed must not come back.
    addEventListener('fired', () => calls.push('fired'), { once: true });
    dispatchEvent(new Event('fired'));
    break;
  case 'signal':
    addEventListener('ping', () => calls.push('signal'), {
      signal: new AbortController().signal,
    });
    break;
  default:
    throw new Error(`unknown MODE ${MODE}`);
}

// Resolves when `settle` is invoked, or to 'timeout' so a lost listener fails the assertion
// instead of hanging the request.
function waitFor(settle) {
  return Promise.race([
    new Promise((resolve) => settle(() => resolve('settled'))),
    scheduler.wait(1000).then(() => 'timeout'),
  ]);
}

async function exercise() {
  switch (MODE) {
    case 'control':
    case 'listeners':
    case 'signal':
      dispatchEvent(new Event('ping'));
      dispatchEvent(new Event('ping'));
      removeEventListener('ping', first);
      dispatchEvent(new Event('ping'));
      dispatchEvent(new Event('fired'));
      break;
    case 'error':
      reportError(new Error('boom'));
      break;
    case 'rejections': {
      const unhandled = waitFor((settle) => (onUnhandled = settle));
      const rejected = Promise.reject(new Error('rejected'));
      calls.push(await unhandled);
      const handled = waitFor((settle) => (onHandled = settle));
      rejected.catch(() => {});
      calls.push(await handled);
      break;
    }
  }
}

async function fetch() {
  await exercise();
  return Response.json({ mode: MODE, snapshotUsed, calls });
}

async function test(ctrl, env) {
  // The runner went through the same pipeline as the cases: whether it was started from a
  // snapshot says whether the pipeline is on in this variant.
  const pipelineOn = snapshotUsed;

  // Cases whose top level the zygote can run: started from the snapshot when the pipeline is on.
  const transferable = {
    control: [],
    error: ['Uncaught Error: boom'],
    rejections: ['unhandled:rejected', 'settled', 'handled', 'settled'],
    listeners: [
      'fired',
      'first',
      'second',
      'third',
      'first',
      'second',
      'second',
    ],
  };
  // Cases whose top level fails the zygote: always started as plain Workers, which run the top
  // level themselves.
  const fallback = {
    signal: ['signal', 'signal', 'signal'],
  };

  const run = async (name, expectedSnapshotUsed, expectedCalls) => {
    const response = await env[name].fetch('http://case/');
    const r = await response.json();
    strictEqual(r.mode, name);
    strictEqual(r.snapshotUsed, expectedSnapshotUsed, `${name}: snapshotUsed`);
    deepStrictEqual(r.calls, expectedCalls, `${name}: calls`);
  };
  for (const [name, expectedCalls] of Object.entries(transferable)) {
    await run(name, pipelineOn, expectedCalls);
  }
  for (const [name, expectedCalls] of Object.entries(fallback)) {
    await run(name, false, expectedCalls);
  }
}

// Only the runner has a test handler: workerd test runs test() of every service that exports one.
export default MODE === 'runner' ? { fetch, test } : { fetch };
