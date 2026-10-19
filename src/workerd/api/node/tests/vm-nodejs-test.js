// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import assert from 'node:assert/strict';
import * as vm from 'node:vm';

// Node v24.15.0, commit 848430679556aed0bd073f2bc263331ad84fa119.
// Bodies retain the assertions from the named parallel tests. The workerd test
// entrypoints replace Node's process-exit and common.mustCall harness.

// test-vm-create-and-run-in-context.js, excluding its GC-specific assertion.
export const createAndRun = {
  test() {
    let context = vm.createContext();
    assert.equal(vm.runInContext('"passed";', context), 'passed');
    const sandbox = { foo: 'bar', thing: 'lala' };
    context = vm.createContext(sandbox);
    assert.equal(context, sandbox);
    assert.equal(context.foo, 'bar');
    assert.equal(context.thing, 'lala');
    vm.runInContext('var foo = 3;', context);
    assert.equal(context.foo, 3);
    assert.equal(context.thing, 'lala');
  },
};

// test-vm-create-context-accessors.js.
export const sandboxAccessors = {
  test() {
    const context = {};
    let value;
    Object.defineProperty(context, 'getter', { get: () => 'ok' });
    Object.defineProperty(context, 'setter', {
      set: (next) => {
        value = next;
      },
      get: () => `ok=${value}`,
    });
    vm.createContext(context);
    const result = vm.runInContext(
      'setter = "test"; [getter, setter]',
      context
    );
    assert.equal(result[0], 'ok');
    assert.equal(result[1], 'ok=test');
  },
};

// test-vm-global-identity.js.
export const globalIdentity = {
  test() {
    const context = vm.createContext();
    context.window = context;
    assert.equal(
      vm.runInContext('this', context),
      vm.runInContext('window', context)
    );
  },
};

// test-vm-data-property-writable.js.
export const propertyDescriptors = {
  test() {
    const context = vm.createContext();
    const descriptor = vm.runInContext(
      `
      Object.defineProperty(this, 'foo', { value: 5 });
      Object.getOwnPropertyDescriptor(this, 'foo');
    `,
      context
    );
    assert.equal(descriptor.writable, false);
    const symbolDescriptor = vm.runInContext(
      `
      const bar = Symbol('bar');
      Object.defineProperty(this, bar, { value: 6 });
      Object.getOwnPropertyDescriptor(this, bar);
    `,
      context
    );
    assert.equal(symbolDescriptor.value, 6);
  },
};

export const separateIntrinsicsAndHostReferences = {
  test() {
    const hostValue = { counter: 0 };
    const callback = (value) => {
      value.counter++;
      return hostValue;
    };
    const context = vm.createContext({ hostValue, callback });
    const other = vm.createContext();
    assert.notEqual(vm.runInContext('Array', context), Array);
    assert.notEqual(vm.runInContext('Object', context), Object);
    assert.notEqual(vm.runInContext('Function', context), Function);
    assert.notEqual(
      vm.runInContext('Array', context),
      vm.runInContext('Array', other)
    );
    assert.equal(vm.runInContext('callback', context), callback);
    assert.equal(vm.runInContext('callback(hostValue)', context), hostValue);
    assert.equal(hostValue.counter, 1);
    vm.runInContext(
      'Object.freeze(Array.prototype); globalThis.localOnly = 1;',
      context
    );
    assert.equal(Object.isFrozen(Array.prototype), false);
    assert.equal(vm.runInContext('typeof localOnly', other), 'undefined');
    assert.equal(Object.hasOwn(globalThis, 'localOnly'), false);
  },
};

export const compileFunctionContext = {
  test() {
    const hostValue = { value: 7 };
    const context = vm.createContext({ hostValue });
    const fn = vm.compileFunction(
      'return [hostValue, value, Array]',
      ['value'],
      {
        parsingContext: context,
      }
    );
    const result = fn(8);
    assert.equal(result[0], hostValue);
    assert.equal(result[1], 8);
    assert.equal(result[2], vm.runInContext('Array', context));
    assert.equal(
      Object.getPrototypeOf(fn),
      vm.runInContext('Function.prototype', context)
    );
  },
};

// test-vm-codegen.js. Context policy can restrict the isolate's eval grant.
export const contextCodeGeneration = {
  test() {
    const context = vm.createContext(
      {},
      { codeGeneration: { strings: false } }
    );
    const EvalError = vm.runInContext('EvalError', context);
    assert.throws(
      () => vm.runInContext('eval("x")', context),
      (error) => error instanceof EvalError
    );
    assert.throws(
      () => vm.runInContext('new Function("return 1")', context),
      (error) => error instanceof EvalError
    );
    assert.throws(
      () => vm.runInContext('eval(undefined)', context),
      (error) => error instanceof EvalError
    );
    const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
    const noWasm = vm.createContext(
      { bytes },
      { codeGeneration: { wasm: false } }
    );
    const CompileError = vm.runInContext('WebAssembly.CompileError', noWasm);
    assert.throws(
      () => vm.runInContext('new WebAssembly.Module(bytes)', noWasm),
      (error) => error instanceof CompileError
    );
    assert.equal(vm.runInContext('eval("1 + 1")', vm.createContext()), 2);
  },
};

// test-vm-module-link.js, simple/depth/circular cases.
export const moduleLinkAndCycle = {
  async test() {
    const context = vm.createContext();
    const foo = new vm.SourceTextModule(
      `
      import getFoo from 'bar';
      export let foo = 42;
      export default getFoo();
    `,
      { context }
    );
    const bar = new vm.SourceTextModule(
      `
      import { foo } from 'foo';
      export default function getFoo() { return foo; }
    `,
      { context }
    );
    let calls = 0;
    await foo.link(async (specifier, module) => {
      calls++;
      assert.equal(foo.status, 'linking');
      if (specifier === 'bar') {
        assert.equal(module, foo);
        return bar;
      }
      assert.equal(specifier, 'foo');
      assert.equal(module, bar);
      return foo;
    });
    assert.equal(calls, 2);
    assert.equal(bar.status, 'linked');
    await foo.evaluate();
    assert.equal(foo.namespace.default, 42);
    assert.equal(foo.namespace, foo.namespace);
  },
};

// test-vm-module-synthetic.js, first live-export case.
export const syntheticLiveExports = {
  async test() {
    const context = vm.createContext();
    const hostValue = {};
    let calls = 0;
    const synthetic = new vm.SyntheticModule(
      ['x', 'hostValue'],
      () => {
        calls++;
        synthetic.setExport('x', 1);
        synthetic.setExport('hostValue', hostValue);
      },
      { context }
    );
    const source = new vm.SourceTextModule(
      `
      import { x, hostValue } from 'synthetic';
      export const getX = () => x;
      export { hostValue };
    `,
      { context }
    );
    await source.link(() => synthetic);
    await source.evaluate();
    assert.equal(calls, 1);
    assert.equal(source.namespace.getX(), 1);
    assert.equal(source.namespace.hostValue, hostValue);
    synthetic.setExport('x', 42);
    assert.equal(source.namespace.getX(), 42);
    await source.evaluate();
    assert.equal(calls, 1);
  },
};

// test-vm-module-import-meta.js, testBasic.
export const moduleImportMeta = {
  async test() {
    const context = vm.createContext();
    const module = new vm.SourceTextModule(
      'globalThis.importMeta = import.meta;',
      {
        context,
        initializeImportMeta(meta, referrer) {
          assert.equal(referrer, module);
          meta.prop = 42;
        },
      }
    );
    await module.link(() => assert.fail('unexpected dependency'));
    await module.evaluate();
    const result = context.importMeta;
    assert.equal(typeof result, 'object');
    assert.equal(Object.getPrototypeOf(result), null);
    assert.equal(result.prop, 42);
    assert.deepEqual(Reflect.ownKeys(result), ['prop']);
  },
};

export const topLevelAwaitAndErrors = {
  async test() {
    const context = vm.createContext();
    const source = new vm.SourceTextModule(
      'export const value = await Promise.resolve(42);',
      { context }
    );
    await source.link(() => assert.fail('unexpected dependency'));
    assert.equal(await source.evaluate(), undefined);
    assert.equal(source.namespace.value, 42);
    const originalNamespace = source.namespace;
    assert.throws(
      () => new vm.SourceTextModule('export const = ;', { context }),
      { name: 'SyntaxError' }
    );
    assert.equal(source.namespace, originalNamespace);
    assert.equal(source.namespace.value, 42);
    const failing = new vm.SourceTextModule(
      'throw new Error("guest failure")',
      { context }
    );
    await failing.link(() => assert.fail('unexpected dependency'));
    await assert.rejects(failing.evaluate(), { message: 'guest failure' });
    assert.equal(failing.status, 'errored');
    assert.equal(failing.error.message, 'guest failure');
  },
};

// test-vm-module-errors.js, checkModuleState and checkLinking.
export const moduleStateAndLinkErrors = {
  async test() {
    const context = vm.createContext();
    const source = new vm.SourceTextModule('export const value = 1;', {
      context,
    });
    assert.throws(() => source.namespace, { code: 'ERR_VM_MODULE_STATUS' });
    await assert.rejects(source.evaluate(), { code: 'ERR_VM_MODULE_STATUS' });
    await source.link(() => assert.fail('unexpected dependency'));
    assert.equal(source.status, 'linked');
    await assert.rejects(
      source.link(() => {}),
      { code: 'ERR_VM_MODULE_ALREADY_LINKED' }
    );
    await source.evaluate();
    assert.throws(() => source.error, { code: 'ERR_VM_MODULE_STATUS' });

    const invalid = new vm.SourceTextModule('import "foo";', { context });
    await assert.rejects(
      invalid.link(() => ({})),
      { code: 'ERR_VM_MODULE_NOT_MODULE' }
    );
    assert.equal(invalid.status, 'errored');
    const different = new vm.SourceTextModule('import "foo";', {
      context: vm.createContext(),
    });
    await assert.rejects(
      different.link(() => source),
      { code: 'ERR_VM_MODULE_DIFFERENT_CONTEXT' }
    );
    assert.equal(different.status, 'errored');
    assert.equal(source.namespace.value, 1);
  },
};

// test-vm-module-dynamic-import.js and test-vm-module-dynamic-namespace.js.
export const moduleDynamicImportCallback = {
  async test() {
    const context = vm.createContext();
    const target = new vm.SourceTextModule('export const value = 42;', {
      context,
    });
    await target.link(() => assert.fail('unexpected target dependency'));
    await target.evaluate();
    let calls = 0;
    for (const returnNamespace of [false, true]) {
      const importer = new vm.SourceTextModule(
        `
          export const promise = import('target', { with: { type: 'json' } });
          export const namespace = await promise;
        `,
        {
          context,
          importModuleDynamically(specifier, referrer, attributes, phase) {
            calls++;
            assert.equal(specifier, 'target');
            assert.equal(referrer, importer);
            assert.equal(Object.getPrototypeOf(attributes), null);
            assert.deepEqual({ ...attributes }, { type: 'json' });
            assert.equal(phase, 'evaluation');
            return returnNamespace ? target.namespace : target;
          },
        }
      );
      await importer.link(() => assert.fail('unexpected static dependency'));
      await importer.evaluate();
      assert.equal(importer.namespace.namespace, target.namespace);
      assert.equal(importer.namespace.namespace.value, 42);
      assert.equal(
        Object.getPrototypeOf(importer.namespace.promise),
        vm.runInContext('Promise.prototype', context)
      );
    }
    assert.equal(calls, 2);

    const missing = new vm.SourceTextModule('await import("target");', {
      context,
    });
    await missing.link(() => assert.fail('unexpected static dependency'));
    await assert.rejects(missing.evaluate(), {
      code: 'ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING',
    });
    assert.equal(missing.status, 'errored');
  },
};

// test-vm-module-basic.js and test-vm-no-dynamic-import-callback.js.
export const scriptAndFunctionDynamicImportCallbacks = {
  async test() {
    const target = new vm.SourceTextModule('export const value = 42;');
    await target.link(() => assert.fail('unexpected target dependency'));
    await target.evaluate();
    let calls = 0;
    for (const kind of ['script', 'function']) {
      let compiled;
      const options = {
        importModuleDynamically(specifier, referrer, attributes, phase) {
          calls++;
          assert.equal(specifier, 'target');
          assert.equal(referrer, compiled);
          assert.equal(Object.getPrototypeOf(attributes), null);
          assert.deepEqual({ ...attributes }, { key: 'value' });
          assert.equal(phase, 'evaluation');
          return target;
        },
      };
      const expression = 'import("target", { with: { key: "value" } })';
      compiled =
        kind === 'script'
          ? new vm.Script(expression, options)
          : vm.compileFunction(`return ${expression}`, [], options);
      const promise =
        kind === 'script' ? compiled.runInThisContext() : compiled();
      assert.equal(Object.getPrototypeOf(promise), Promise.prototype);
      assert.equal(await promise, target.namespace);
    }
    assert.equal(calls, 2);
    await assert.rejects(new vm.Script('import("target")').runInThisContext(), {
      code: 'ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING',
    });
    await assert.rejects(vm.compileFunction('return import("target")')(), {
      code: 'ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING',
    });
  },
};

// test-vm-module-basic.js, modules without an explicit context.
export const moduleInMainContext = {
  async test() {
    const dependency = new vm.SourceTextModule('export let value = 42;');
    const source = new vm.SourceTextModule(`
      import { value } from 'dependency';
      export { value };
      export const global = globalThis;
      export const array = Array;
      export const makeObject = () => ({});
    `);
    assert.equal(source.context, undefined);
    await source.link((specifier, referrer) => {
      assert.equal(specifier, 'dependency');
      assert.equal(referrer, source);
      return dependency;
    });
    assert.equal(await source.evaluate(), undefined);
    assert.equal(source.status, 'evaluated');
    assert.equal(source.namespace.value, 42);
    assert.equal(source.namespace.global, globalThis);
    assert.equal(source.namespace.array, Array);
    assert.equal(
      Object.getPrototypeOf(source.namespace.makeObject()),
      Object.prototype
    );
    assert.equal(
      Object.getPrototypeOf(source.namespace.makeObject),
      Function.prototype
    );
  },
};

// test-vm-module-link-shared-deps.js and test-vm-module-link.js, circular2.
export const asynchronousDiamondAndCycle = {
  async test() {
    const context = vm.createContext({ evaluations: 0 });
    const sources = {
      root: `
        import * as left from 'left';
        import * as right from 'right';
        export { left, right };
        export function initialValue() { return 40; }
      `,
      left: `
        import * as shared from 'shared';
        export { shared };
        export function read() { return shared.value; }
      `,
      right: `
        import * as shared from 'shared';
        export { shared };
        export function read() { return shared.value; }
      `,
      shared: `
        import { initialValue } from 'root';
        globalThis.evaluations++;
        export let value = initialValue();
        export function increment() { return ++value; }
      `,
    };
    const modules = new Map(
      Object.entries(sources).map(([identifier, source]) => [
        identifier,
        new vm.SourceTextModule(source, { context, identifier }),
      ])
    );
    const root = modules.get('root');
    const requests = [];
    await root.link(async (specifier, referrer) => {
      assert.equal(modules.get(referrer.identifier), referrer);
      requests.push(`${referrer.identifier}->${specifier}`);
      // Defer resolution so both branches can encounter the shared dependency.
      await Promise.resolve();
      assert.ok(modules.has(specifier));
      return modules.get(specifier);
    });
    assert.deepEqual(requests.sort(), [
      'left->shared',
      'right->shared',
      'root->left',
      'root->right',
      'shared->root',
    ]);
    for (const module of modules.values())
      assert.equal(module.status, 'linked');
    await root.evaluate();
    const { left, right } = root.namespace;
    assert.equal(left.shared, right.shared);
    assert.equal(left.shared, modules.get('shared').namespace);
    assert.equal(left.read(), 40);
    assert.equal(right.read(), 40);
    assert.equal(left.shared.increment(), 41);
    assert.equal(left.read(), 41);
    assert.equal(right.read(), 41);
    assert.equal(context.evaluations, 1);
    await root.evaluate();
    assert.equal(context.evaluations, 1);
  },
};

export const synchronousModuleHostCallbacks = {
  async test() {
    const calls = [];
    const fillRandom = (bytes) => {
      calls.push('source');
      assert.equal(crypto.getRandomValues(bytes), bytes);
      return bytes;
    };
    const context = vm.createContext({ fillRandom });
    const synthetic = new vm.SyntheticModule(
      ['value'],
      () => {
        calls.push('synthetic');
        const bytes = new Uint8Array(8);
        assert.equal(crypto.getRandomValues(bytes), bytes);
        synthetic.setExport('value', Promise.resolve(bytes));
      },
      { context }
    );
    const source = new vm.SourceTextModule(
      `
      import { value } from 'synthetic';
      export const sourceBytes = fillRandom(new Uint8Array(8));
      export const syntheticBytes = await value;
    `,
      { context }
    );
    await source.link((specifier) => {
      assert.equal(specifier, 'synthetic');
      return synthetic;
    });
    await source.evaluate();
    assert.deepEqual(calls, ['synthetic', 'source']);
    assert.equal(source.namespace.sourceBytes.byteLength, 8);
    assert.equal(source.namespace.syntheticBytes.byteLength, 8);
  },
};
