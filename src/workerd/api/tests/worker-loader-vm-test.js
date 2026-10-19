// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert/strict';
import * as vm from 'node:vm';

const sources = new Map([
  [
    'cycle-a',
    `
      import * as base from 'base';
      import { readThroughCycle } from 'cycle-b';
      globalThis.evaluations = (globalThis.evaluations || 0) + 1;
      globalThis.order = ['before await'];
      export function read() { return base.value; }
      export const initial = await Promise.resolve().then(() => {
        order.push('awaited');
        return readThroughCycle();
      });
      order.push('after await');
      export function advance() {
        base.callback(base.state);
        base.increment();
        return [readThroughCycle(), base.next()];
      }
      export { base };
      export const metadata = import.meta;
      export const sameMetadata = import.meta === import.meta;
    `,
  ],
  [
    'cycle-b',
    `
      import { read } from 'cycle-a';
      export function readThroughCycle() { return read(); }
    `,
  ],
  ['invalid', 'export const = ;'],
  [
    'request-io',
    `
      export const bytes = fillRandom(new Uint8Array(8));
      export const source = await sourceBroker.getSource('later');
    `,
  ],
  [
    'link-failure',
    `
      import { missing, increment } from 'base';
      increment();
      export { missing };
    `,
  ],
  [
    'evaluation-failure',
    `
      import { callback, state, increment } from 'base';
      callback(state);
      increment();
      throw new Error('late evaluation failure');
    `,
  ],
  [
    'later',
    `
      import * as base from 'base';
      import * as first from 'cycle-a';
      export { base, first };
      export function read() { return base.value; }
    `,
  ],
]);

export class SourceBroker extends WorkerEntrypoint {
  getSource(identifier) {
    assert.ok(sources.has(identifier));
    return sources.get(identifier);
  }
}

async function runInChild(sourceBroker) {
  const hostState = { counter: 0 };
  const callback = (state) => {
    assert.equal(state, hostState);
    state.counter++;
    return state;
  };
  let synchronousHostCalls = 0;
  const fillRandom = (bytes) => {
    synchronousHostCalls++;
    assert.equal(crypto.getRandomValues(bytes), bytes);
    return bytes;
  };
  const context = vm.createContext({
    hostState,
    callback,
    sourceBroker,
    fillRandom,
  });
  const otherContext = vm.createContext();
  const contextGlobal = vm.runInContext('globalThis', context);
  vm.runInContext(
    `globalThis.next = (() => { let value = 0; return () => ++value; })();`,
    context
  );
  const closure = vm.runInContext('next', context);
  assert.equal(closure(), 1);
  assert.equal(vm.runInContext('callback(hostState)', context), hostState);

  for (const name of ['Array', 'Object', 'Function']) {
    assert.notEqual(vm.runInContext(name, context), globalThis[name]);
    assert.notEqual(
      vm.runInContext(name, context),
      vm.runInContext(name, otherContext)
    );
    vm.runInContext(
      `Object.freeze(${name}.prototype); Object.freeze(${name});`,
      context
    );
    assert.equal(Object.isFrozen(globalThis[name].prototype), false);
    assert.equal(Object.isFrozen(globalThis[name]), false);
    assert.equal(
      vm.runInContext(`Object.isFrozen(${name}.prototype)`, otherContext),
      false
    );
  }
  const hostArray = [];
  hostArray.push(Object.assign({}, { value: (() => 42)() }));
  assert.equal(hostArray[0].value, 42);

  const base = new vm.SourceTextModule(
    `
      export const state = hostState;
      export const callback = globalThis.callback;
      export const next = globalThis.next;
      export let value = 1;
      export function increment() { return ++value; }
    `,
    { context, identifier: 'base' }
  );
  await base.link(() => assert.fail('unexpected base dependency'));
  await base.evaluate();
  const baseNamespace = base.namespace;
  assert.equal(baseNamespace.state, hostState);
  assert.equal(baseNamespace.callback, callback);
  assert.equal(baseNamespace.next, closure);

  // The invocation stays active across source RPCs. No context, callback, module,
  // or namespace crosses the worker boundary.
  const modules = new Map([['base', base]]);
  let metadataCalls = 0;
  for (const identifier of ['cycle-a', 'cycle-b']) {
    const source = await sourceBroker.getSource(identifier);
    assert.equal(typeof source, 'string');
    modules.set(
      identifier,
      new vm.SourceTextModule(source, {
        context,
        identifier,
        initializeImportMeta(meta, module) {
          metadataCalls++;
          meta.url = module.identifier;
        },
      })
    );
  }
  const linker = (specifier) => {
    assert.ok(modules.has(specifier));
    return modules.get(specifier);
  };
  const first = modules.get('cycle-a');
  await first.link(linker);
  await first.evaluate();
  const firstNamespace = first.namespace;
  assert.equal(firstNamespace.initial, 1);
  assert.equal(firstNamespace.base, baseNamespace);
  assert.equal(firstNamespace.metadata.url, 'cycle-a');
  assert.equal(Object.getPrototypeOf(firstNamespace.metadata), null);
  assert.equal(firstNamespace.sameMetadata, true);
  assert.equal(metadataCalls, 1);
  assert.deepEqual(Array.from(context.order), [
    'before await',
    'awaited',
    'after await',
  ]);
  assert.deepEqual(Array.from(firstNamespace.advance()), [2, 2]);
  assert.equal(hostState.counter, 2);
  await first.evaluate();
  assert.equal(first.namespace, firstNamespace);
  assert.equal(context.evaluations, 1);

  const invalidSource = await sourceBroker.getSource('invalid');
  assert.throws(() => new vm.SourceTextModule(invalidSource, { context }), {
    name: 'SyntaxError',
  });
  assert.equal(vm.runInContext('globalThis', context), contextGlobal);
  assert.equal(vm.runInContext('next', context), closure);
  assert.equal(base.namespace, baseNamespace);
  assert.equal(baseNamespace.callback, callback);
  assert.equal(baseNamespace.state, hostState);
  assert.equal(first.namespace, firstNamespace);
  assert.deepEqual(Array.from(firstNamespace.advance()), [3, 3]);

  const later = new vm.SourceTextModule(await sourceBroker.getSource('later'), {
    context,
    identifier: 'later',
  });
  await later.link(linker);
  await later.evaluate();
  assert.equal(later.namespace.first, firstNamespace);
  assert.equal(later.namespace.base, baseNamespace);
  assert.equal(baseNamespace.increment(), 4);
  assert.equal(later.namespace.read(), 4);
  assert.equal(context.evaluations, 1);

  const requestIo = new vm.SourceTextModule(
    await sourceBroker.getSource('request-io'),
    { context }
  );
  await requestIo.link(() => assert.fail('unexpected request-io dependency'));
  await requestIo.evaluate();
  assert.equal(synchronousHostCalls, 1);
  assert.equal(requestIo.namespace.bytes.byteLength, 8);
  const laterSource = await sourceBroker.getSource('later');
  assert.equal(requestIo.namespace.source, laterSource);

  let syntheticCalls = 0;
  const synthetic = new vm.SyntheticModule(
    ['sourcePromise', 'state'],
    () => {
      syntheticCalls++;
      // Start the RPC synchronously in the evaluator, before any promise turn.
      synthetic.setExport('sourcePromise', sourceBroker.getSource('later'));
      synthetic.setExport('state', hostState);
    },
    { context }
  );
  const syntheticConsumer = new vm.SourceTextModule(
    `
      import { sourcePromise, state } from 'request-io-synthetic';
      export { state };
      export const source = await sourcePromise;
    `,
    { context }
  );
  await syntheticConsumer.link((specifier) => {
    assert.equal(specifier, 'request-io-synthetic');
    return synthetic;
  });
  await syntheticConsumer.evaluate();
  assert.equal(syntheticCalls, 1);
  assert.equal(syntheticConsumer.namespace.source, laterSource);
  assert.equal(syntheticConsumer.namespace.state, hostState);

  const assertRetainedState = (value, counter) => {
    assert.equal(vm.runInContext('globalThis', context), contextGlobal);
    assert.equal(vm.runInContext('next', context), closure);
    assert.equal(base.namespace, baseNamespace);
    assert.equal(baseNamespace.callback, callback);
    assert.equal(baseNamespace.state, hostState);
    assert.equal(first.namespace, firstNamespace);
    assert.equal(later.namespace.first, firstNamespace);
    assert.equal(later.namespace.base, baseNamespace);
    assert.equal(later.namespace.read(), value);
    assert.equal(hostState.counter, counter);
    assert.equal(context.evaluations, 1);
  };
  const linkFailure = new vm.SourceTextModule(
    await sourceBroker.getSource('link-failure'),
    { context }
  );
  await assert.rejects(linkFailure.link(linker), { name: 'SyntaxError' });
  assertRetainedState(4, 3);
  assert.deepEqual(Array.from(firstNamespace.advance()), [5, 4]);

  const evaluationFailure = new vm.SourceTextModule(
    await sourceBroker.getSource('evaluation-failure'),
    { context }
  );
  await evaluationFailure.link(linker);
  assert.equal(evaluationFailure.status, 'linked');
  await assert.rejects(evaluationFailure.evaluate(), {
    message: 'late evaluation failure',
  });
  assert.equal(evaluationFailure.status, 'errored');
  // Evaluation effects persist even when the new module throws.
  assertRetainedState(6, 5);
  await assert.rejects(evaluationFailure.evaluate(), {
    message: 'late evaluation failure',
  });
  assertRetainedState(6, 5);
  assert.deepEqual(Array.from(firstNamespace.advance()), [7, 5]);
  assertRetainedState(7, 6);
  return {
    counter: hostState.counter,
    closure: closure(),
    value: later.namespace.read(),
    evaluations: context.evaluations,
  };
}

export const lateSourceInActiveInvocation = {
  async test(_ctrl, env, ctx) {
    const worker = env.loader.load({
      compatibilityDate: '2026-10-01',
      compatibilityFlags: [
        'nodejs_compat',
        'enable_nodejs_vm_module',
        'enable_nodejs_vm_context',
        'allow_insecure_inefficient_logged_eval',
      ],
      // This eval grant is a local test facility, not a hosted loader contract.
      allowExperimental: true,
      mainModule: 'child.js',
      modules: {
        'child.js': `
          import { WorkerEntrypoint } from 'cloudflare:workers';
          import assert from 'node:assert/strict';
          import * as vm from 'node:vm';
          const runInChild = ${runInChild.toString()};
          export default class extends WorkerEntrypoint {
            run() { return runInChild(this.env.sourceBroker); }
          }
        `,
      },
      env: { sourceBroker: ctx.exports.SourceBroker({}) },
      globalOutbound: null,
    });
    assert.deepEqual(await worker.getEntrypoint().run(), {
      counter: 6,
      closure: 6,
      value: 7,
      evaluations: 1,
    });
  },
};
