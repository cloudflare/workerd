import {
  default as assert,
  notStrictEqual,
  ok,
  rejects,
  strictEqual,
  deepStrictEqual,
} from 'assert'; // Intentionally omit the 'node:' prefix
import { foo, default as def } from 'foo';
import { default as fs } from 'node:fs';
import { Buffer } from 'buffer'; // Intentionally omit the 'node:' prefix
import { foo as foo2, default as def2 } from 'bar';
import { createRequire } from 'module'; // Intentionally omit the 'node:' prefix

import * as workers from 'cloudflare:workers';
strictEqual(typeof workers, 'object');
strictEqual(typeof workers.DurableObject, 'function');
strictEqual(typeof workers.RpcPromise, 'function');
strictEqual(typeof workers.RpcProperty, 'function');
strictEqual(typeof workers.RpcStub, 'function');
strictEqual(typeof workers.RpcTarget, 'function');
strictEqual(typeof workers.ServiceStub, 'function');
strictEqual(typeof workers.WorkerEntrypoint, 'function');
strictEqual(typeof workers.WorkflowEntrypoint, 'function');
strictEqual(typeof workers.waitUntil, 'function');
strictEqual(typeof workers.withEnv, 'function');
strictEqual(typeof workers.env, 'object');

await rejects(import('cloudflare-internal:env'), {
  message: /Module not found/,
});
await rejects(import('cloudflare-internal:filesystem'));

// Verify that import.meta.url is correct here.
strictEqual(import.meta.url, 'file:///bundle/worker');

// Verify that import.meta.main is true here.
ok(import.meta.main);

// When running in nodejs_compat_v2 mode, the globalThis.Buffer
// and globalThis.process properties are resolved from the module
// registry. Let's make sure we get good values here.
strictEqual(typeof globalThis.Buffer, 'function');
strictEqual(globalThis.Buffer, Buffer);
ok(globalThis.process);
strictEqual(typeof globalThis.process, 'object');

// Verify that process.getBuiltinModule works correctly with
// the new module registry.
const builtinBuffer1 = process.getBuiltinModule('buffer');
const builtinBuffer2 = process.getBuiltinModule('node:buffer');
strictEqual(builtinBuffer1, builtinBuffer2);
strictEqual(builtinBuffer1.Buffer, globalThis.Buffer);
const nonExistent = process.getBuiltinModule('non-existent');
strictEqual(nonExistent, undefined);

// Our internal implementation of console.log depend on the module registry
// to get the node-internal:internal_inspect module. This console.log makes
// sure that works correctly without crashing when using the new module
// registry implementation.
console.log(import.meta);

// Verify that import.meta.resolve provides correct results here.
// The input should be interpreted as a URL and normalized according
// to the rules in the WHATWG URL specification.
strictEqual(import.meta.resolve('./.././test/.././.%2e/foo'), 'file:///foo');
strictEqual(import.meta.resolve('foo'), 'file:///bundle/foo');

// There are four tests at this top level... one for the import of the node:assert
// module without the node: prefix specifier, two for the imports of the foo and
// bar modules from the worker, and one for the aliases node:fs module from the
// module worker.

strictEqual(foo, 1);
strictEqual(def, 2);
strictEqual(foo2, 1);
strictEqual(def2, 2);
strictEqual(fs, 'abc');

// Equivalent to the above, but using the file: URL scheme.
import { foo as foo3, default as def3 } from 'file:///bundle/foo';
strictEqual(foo, foo3);
strictEqual(def, def3);

import * as text from 'text';
strictEqual(text.default, 'abc');

import * as data from 'data';
strictEqual(Buffer.from(data.default).toString(), 'abcdef');

import * as json from 'json';
deepStrictEqual(json.default, { foo: 1 });

// Synchronously resolved promises can be awaited.
await Promise.resolve();

// CommonJS modules can be imported and should work as expected.
import { default as cjs2 } from 'cjs2';
strictEqual(cjs2.foo, 1);
strictEqual(cjs2.bar, 2);
strictEqual(cjs2.filename, 'cjs1');
strictEqual(cjs2.dirname, '/bundle');
strictEqual(cjs2.assert, assert);

// CommonJS modules can define named exports.
import { foo as cjs1foo, bar as cjs1bar } from 'cjs1';
strictEqual(cjs1foo, 1);
strictEqual(cjs1bar, 2);

// The createRequire API works as expected.
const myRequire = createRequire(import.meta.url);
const customRequireCjs = myRequire('cjs1');
strictEqual(customRequireCjs.foo, cjs1foo);
strictEqual(customRequireCjs.bar, cjs1bar);

const customRequireCjs2 = myRequire(import.meta.resolve('cjs2'));
strictEqual(customRequireCjs2.foo, cjs2.foo);

// When the module being imported throws an error during evaluation,
// the error is propagated correctly.
await rejects(import('file:///bundle/cjs3'), {
  message: 'boom',
});

// The modules cjs4 and cjs5 have a circular dependency on each other.
// They should both load without error.
import { default as cjs4 } from 'cjs4';
import { default as cjs5 } from 'cjs5';
deepStrictEqual(cjs4, {});
deepStrictEqual(cjs5, {});

// These dynamics imports can be top-level awaited because they
// are immediately rejected with errors.
await rejects(import('invalid-json'), {
  message: /Unexpected non-whitespace character after JSON/,
});

await rejects(import('module-not-found'), {
  message: /Module not found: file:\/\/\/bundle\/module-not-found/,
});

// Verify that a module is unable to perform IO operations at the top level, even if
// the dynamic import is initiated within the scope of an active IoContext.
export const noTopLevelIo = {
  async test() {
    await rejects(import('bad'), {
      message: /^Disallowed operation called within global scope/,
    });
  },
};

// Verify that async local storage is propagated into dynamic imports.
export const alsPropagationDynamicImport = {
  async test() {
    const { AsyncLocalStorage } = await import('async_hooks');
    globalThis.als = new AsyncLocalStorage();
    const res = await globalThis.als.run(123, () => import('als'));
    strictEqual(res.default, 123);
    delete globalThis.als;
  },
};

// Query strings and fragments create new instances of known modules.
export const queryAndFragment = {
  async test() {
    // Each resolves the same underlying module but creates a new instance.
    // The exports should be the same but the module namespaces should be different.

    const a = await import('foo?query');
    const b = await import('foo#fragment');
    const c = await import('foo?query#fragment');
    const d = await import('foo');

    strictEqual(a.default, 2);
    strictEqual(a.foo, 1);
    strictEqual(a.default, b.default);
    strictEqual(a.default, c.default);
    strictEqual(a.default, d.default);
    strictEqual(a.foo, b.foo);
    strictEqual(a.foo, c.foo);
    strictEqual(a.foo, d.foo);

    notStrictEqual(a, b);
    notStrictEqual(a, c);
    notStrictEqual(a, d);
    notStrictEqual(b, c);
    notStrictEqual(b, d);
    notStrictEqual(c, d);

    // The import.meta.url for each should match the specifier used to import the instance.
    strictEqual(a.bar, 'file:///bundle/foo?query');
    strictEqual(b.bar, 'file:///bundle/foo#fragment');
    strictEqual(c.bar, 'file:///bundle/foo?query#fragment');
    strictEqual(d.bar, 'file:///bundle/foo');
  },
};

// We do not currently support import attributes. Per the recommendation
// in the spec, we throw an error when they are encountered.
export const importAssertionsFail = {
  async test() {
    await rejects(import('ia'), {
      message: /^Import attributes are not supported/,
    });
    await rejects(import('foo', { with: { a: 'abc' } }), {
      message: /^Import attributes are not supported/,
    });
  },
};

export const invalidUrlAsSpecifier = {
  async test() {
    await rejects(import('zebra: not a \x00 valid URL'), {
      message: /Module not found/,
    });
  },
};

export const evalErrorsInEsmTopLevel = {
  async test() {
    await rejects(import('esm-error'), {
      message: /boom/,
    });
    await rejects(import('esm-error-dynamic'), {
      message: /boom/,
    });
  },
};

export const wasmModuleTest = {
  async test() {
    const { default: wasm } = await import('wasm');
    ok(wasm instanceof WebAssembly.Module);
    await WebAssembly.instantiate(wasm, {});
  },
};

import source wasmSource from 'wasm';
export const wasmSourcePhaseTest = {
  async test() {
    ok(wasmSource instanceof WebAssembly.Module);
    await WebAssembly.instantiate(wasmSource, {});
  },
};

export const wasmDynamicSourcePhaseTest = {
  async test() {
    const wasmSource = await import.source('wasm');
    ok(wasmSource instanceof WebAssembly.Module);
    await WebAssembly.instantiate(wasmSource, {});
  },
};

export const wasmDynamicSourcePhaseFailureTest = {
  async test() {
    await rejects(import.source('foo'), {
      message:
        'Source phase import not available for module: file:///bundle/foo',
    });
  },
};

export const complexModuleTest = {
  async test() {
    const { abc } = await import('complex');
    strictEqual(abc.foo, 1);
    strictEqual(abc.def.bar, 2);

    const { default: abc2 } = await import('abc');
    strictEqual(abc2, 'file:///bundle/abc');
  },
};

// TODO(now): Tests
// * [x] Include tests for all known module types
//   * [x] ESM
//   * [x] CommonJS
//   * [x] Text
//   * [x] Data
//   * [x] JSON
//   * [x] WASM
//   * [x] Python (works, but still needs to be fully tested)
// * [x] IO is forbidden in top-level module scope
// * [x] Async local storage context is propagated into dynamic imports
// * [x] Static import correctly handles node: modules with/without the node: prefix
// * [x] Dynamic import correctly handles node: modules with/without the node: prefix
// * [x] Worker bundle can alias node: modules
// * [x] modules not found are correctly reported as errors
// * [x] Errors during ESM evaluation are correctly reported
// * [x] Errors during dynamic import are correctly reported
// * [x] Errors in JSON module parsing are correctly reported
// * [x] Module specifiers are correctly handled as URLs
//   * [x] Querys and fragments resolve new instances of known modules
//   * [x] URL resolution works correctly
//   * [x] Invalid URLs are correctly reported as errors
// * [x] Import attributes should be rejected
// * [x] require(...) Works in CommonJs Modules
// * [x] require(...) correctly handles node: modules with/without the node: prefix
// * [x] Circular dependencies are correctly handled
// * [x] Errors during CommonJs evaluation are correctly reported
// * [x] CommonJs modules correctly expose named exports
// * [x] require('module').createRequire API works as expected
// * [x] Entry point ESM with no default export is correctly reported as error
// * [ ] Fallback service works as expected
// * [x] console.log output correctly uses node-internal:inspect for output
// ...
