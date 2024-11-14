import {
  notStrictEqual,
  ok,
  rejects,
  strictEqual,
  deepStrictEqual,
} from 'assert';
import { foo, default as def } from 'foo';
import { default as fs } from 'node:fs';
import { Buffer } from 'buffer';
import { foo as foo2, default as def2 } from 'bar';

// Verify that import.meta.url is correct here.
strictEqual(import.meta.url, 'file:///worker');

// Verify that import.meta.main is true here.
ok(import.meta.main);

// Verify that import.meta.resolve provides correct results here.
// The input should be interpreted as a URL and normalized according
// to the rules in the WHATWG URL specification.
strictEqual(import.meta.resolve('./.././test/.././../foo'), 'file:///foo');

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
import { foo as foo3, default as def3 } from 'file:///foo';
strictEqual(foo, foo3);
strictEqual(def, def3);

import * as text from 'text';
strictEqual(text.default, 'abc');

import * as data from 'data';
strictEqual(Buffer.from(data.default).toString(), 'abcdef');

import * as json from 'json';
deepStrictEqual(json.default, { foo: 1 });

await rejects(import('invalid-json'), {
  message: /Unexpected non-whitespace character after JSON/,
});

await rejects(import('module-not-found'), {
  message: /Module not found: file:\/\/\/module-not-found/,
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
    strictEqual(a.bar, 'file:///foo?query');
    strictEqual(b.bar, 'file:///foo#fragment');
    strictEqual(c.bar, 'file:///foo?query#fragment');
    strictEqual(d.bar, 'file:///foo');
  },
};

// We do not currently support import assertions/attributes. Per the recommendation
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

// TODO(now): Tests
// * [ ] Include tests for all known module types
//   * [x] ESM
//   * [ ] CommonJS
//   * [x] Text
//   * [x] Data
//   * [x] JSON
//   * [ ] WASM
//   * [ ] Python
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
// * [x] Import assertions should be rejected
// * [ ] require(...) Works in CommonJs Modules
// * [ ] require(...) correctly handles node: modules with/without the node: prefix
// * [ ] Circular dependencies are correctly handled
// * [ ] Errors during CommonJs evaluation are correctly reported
// * [ ] Entry point ESM with no default export is correctly reported as error
// * [ ] CommonJs modules correctly expose named exports
// * [ ] require('module').createRequire API works as expected
// * [ ] Fallback service works as expected
// * [ ] console.log output correctly uses node-internal:inspect for output
// ...
