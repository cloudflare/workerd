// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  default as assert,
  notStrictEqual,
  ok,
  rejects,
  strictEqual,
  throws,
  deepStrictEqual,
} from 'assert'; // Intentionally omit the 'node:' prefix
import { foo, default as def } from 'foo';
import { default as fs } from 'node:fs';
import { Buffer } from 'buffer'; // Intentionally omit the 'node:' prefix
import { foo as foo2, default as def2 } from 'bar';
import { createRequire } from 'module'; // Intentionally omit the 'node:' prefix
import { default as processStatic } from 'node:process';

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
strictEqual(typeof workers.onEvict, 'symbol');
strictEqual(typeof workers.waitUntil, 'function');
strictEqual(typeof workers.withEnv, 'function');
strictEqual(typeof workers.env, 'object');
strictEqual(typeof workers.cache, 'object');

await rejects(import('cloudflare-internal:env'), {
  message: /Module not found/,
});
await rejects(import('cloudflare-internal:filesystem'));

// Verify that import.meta.url is correct here.
strictEqual(import.meta.url, 'file:///bundle/worker');

// Verify that import.meta.main is true here.
ok(import.meta.main);

// Verify that import.meta.filename and import.meta.dirname are correct
// for file: URLs (WinterCG import.meta path helpers).
strictEqual(import.meta.filename, '/bundle/worker');
strictEqual(import.meta.dirname, '/bundle');

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

// import.meta.resolve must be equivalent to `new URL(specifier, import.meta.url).href`.
// The URL parser collapses dot segments (including percent-encoded "%2e" forms, as
// exercised above) but must NOT percent-decode already-encoded unreserved bytes:
// "%66" (an encoded 'f') stays "%66" rather than becoming "f". This matches Node.js
// and the HTML specification. See importMeta in jsg/modules-new.c++.
strictEqual(import.meta.resolve('./%66oo.js'), 'file:///bundle/%66oo.js');
strictEqual(import.meta.resolve('./a/../%66oo.js'), 'file:///bundle/%66oo.js');

// import.meta.resolve throws (never returns null) for an unparseable specifier.
throws(() => import.meta.resolve('https://[bad'), { name: 'TypeError' });

// A bare specifier naming a Node.js built-in resolves to its canonical "node:"
// URL — matching import()/require() and Node's own import.meta.resolve — rather
// than being treated as a bare path relative to the module's base URL (which
// would incorrectly yield 'file:///bundle/fs').
strictEqual(import.meta.resolve('fs'), 'node:fs');
strictEqual(import.meta.resolve('node:fs'), 'node:fs');
// The deprecated "sys" alias maps to node:util, as it does elsewhere.
strictEqual(import.meta.resolve('sys'), 'node:util');
// Calling import.meta.resolve with no argument is a TypeError (Node parity),
// not a silent resolution of the coerced string "undefined".
throws(() => import.meta.resolve(), { name: 'TypeError' });

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
// __filename is the absolute path of the module file, matching Node.js
// (path.dirname(__filename) === __dirname).
strictEqual(cjs2.filename, '/bundle/cjs1');
strictEqual(cjs2.dirname, '/bundle');
strictEqual(cjs2.assert, assert);

// CommonJS modules can define named exports.
import { foo as cjs1foo, bar as cjs1bar } from 'cjs1';
strictEqual(cjs1foo, 1);
strictEqual(cjs1bar, 2);

// Duplicate named exports, including the implicit default export, must not be passed to V8.
import duplicateExports, { foo as duplicateFoo } from 'cjs-duplicate-exports';
deepStrictEqual(duplicateExports, { foo: 1 });
strictEqual(duplicateFoo, 1);

// The createRequire API works as expected.
const myRequire = createRequire(import.meta.url);
const customRequireCjs = myRequire('cjs1');
strictEqual(customRequireCjs.foo, cjs1foo);
strictEqual(customRequireCjs.bar, cjs1bar);

const customRequireCjs2 = myRequire(import.meta.resolve('cjs2'));
strictEqual(customRequireCjs2.foo, cjs2.foo);

// Node's require(esm) convention: a string-named 'module.exports' export
// controls what require() returns for an ES module. Without it, require(esm)
// returns the module namespace.
deepStrictEqual(myRequire('esm-module-exports'), { hello: 'world' });

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

// Dynamic import() works from within a CommonJS module, resolving the
// specifier relative to the CJS module itself (its script origin is the
// module's canonical URL).
import { default as cjsDyn } from 'cjs-dyn';
{
  const ns = await cjsDyn.relative;
  strictEqual(ns.foo, 1);
  strictEqual(ns.default, 2);
  const nodeNs = await cjsDyn.builtin;
  strictEqual(typeof nodeNs.default.ok, 'function');
}

// These dynamics imports can be top-level awaited because they
// are immediately rejected with errors.
await rejects(import('invalid-json'), {
  message: /Unexpected non-whitespace character after JSON/,
});

await rejects(import('module-not-found'), {
  message: /Module not found: file:\/\/\/bundle\/module-not-found/,
});

await rejects(import('file:///outside'), {
  message: /Module not found/,
});

await import('file:///bundle/outside');

const abc123 = await import('abc123');
strictEqual(abc123.default, 1);

// Full URLs can be used as module specifiers. These would not
// actually resolve to network requests.
const mod = await import('https://example.com/mod');
strictEqual(mod.default, 'example');

// Whitespace and leading ./, ../, and multiple slashes are
// stripped and ignored when setting up the module registry,
// so they should resolve as expected here as being relative
// to the bundle base URL.
const mod2 = await import('   ./should/be/ok   ');
strictEqual(mod2.default, 1);

// UTF-8 percent-encoded of 部品 (Japanese for "component")
const mod3 = await import('%E9%83%A8%E5%93%81');
// Non-ASCII specifiers resolve identically whether written as raw characters
// in the source text or as escape sequences.
const mod4 = await import('部品');
const mod5 = await import('\u90e8\u54c1');
import { default as mod7 } from '部品';
import { default as mod8 } from '\u90e8\u54c1';
import { default as mod10 } from '%E9%83%A8%E5%93%81';

strictEqual(mod3.default, 1);
strictEqual(mod4.default, 1);
strictEqual(mod5.default, 1);
strictEqual(mod7, 1);
strictEqual(mod8, 1);
strictEqual(mod10, 1);

// A Latin-1 string of the UTF-8 *bytes* of 部品 is not the same specifier as
// 部品: each char is its own code point and percent-encodes individually
// (%C3%A9%C2%83... rather than %E9%83%A8...), matching Node.js and browsers.
await rejects(import('\xE9\x83\xA8\xE5\x93\x81'), {
  message: /Module not found/,
});

// require() must resolve the same specifier forms as import().
strictEqual(myRequire('部品').default, 1);
strictEqual(myRequire('%E9%83%A8%E5%93%81').default, 1);
throws(() => myRequire('\xE9\x83\xA8\xE5\x93\x81'), {
  message: /Module not found/,
});

// Source text is decoded as UTF-8: non-ASCII string literals, identifiers, and
// template literals round-trip exactly, both for Latin-1-representable text
// and for text requiring UTF-16.
const latin1Src = await import('latin1-src');
strictEqual(latin1Src.default, 'à la carte');
const unicodeSrc = await import('unicode-src');
strictEqual(unicodeSrc.default, 'café 部品 🎉');
strictEqual(unicodeSrc.tpl, '→café 部品 🎉←');

// The percent-encoded UTF-16 form of 部品 should not work.
await rejects(import('%E8%90%C1%54'), {
  message: /Module not found/,
});
await rejects(import('%90%E8%54%C1'), {
  message: /Module not found/,
});

// The cjs6 module attempts to require and esm with a top-level await, which is rejected
// following node.js' established require(esm) precedent.
await rejects(import('cjs6'), {
  message: /^Top-level await is not supported/,
});

// Cannot directly require an ESM with top-level await either. This must hold
// even after the module has already been fully evaluated by a prior import:
// require()'s top-level-await rejection must not depend on evaluation order
// (regression -- the already-evaluated early-return used to skip the check).
await import('tla');
throws(() => myRequire('tla'), {
  message: /^Top-level await is not supported/,
});

// Verify that a module is unable to perform IO operations at the top level, even if
// the dynamic import is initiated within the scope of an active IoContext.
export const nestedRequireDoesNotCrashSiblingTlaModule = {
  async test() {
    // Before the evaluation-depth scope covered the eval callback, this aborted the process.
    const mod = await import('tla-entry');
    strictEqual(mod.default, 1);
  },
};

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

// Unrecognized import attributes are rejected.
export const importAssertionsFail = {
  async test() {
    await rejects(import('ia'), {
      message: /^Unsupported import attribute: "a"/,
    });
    await rejects(import('foo', { with: { a: 'abc' } }), {
      message: /^Unsupported import attribute: "a"/,
    });
  },
};

// V8 hands static imports [key, value, source_offset, ...] but dynamic imports
// [key, value, ...]. Reading the dynamic array with a stride of 3 walked off the
// end of it, so every case below needs at least two attributes to be meaningful.
export const multipleImportAttributes = {
  async test() {
    // The second attribute's key must be the one reported, not its value, and not
    // the value of some other attribute.
    await rejects(import('json', { with: { type: 'json', a: 'b' } }), {
      message: /^Unsupported import attribute: "a"/,
    });

    // With the array length at 4, a stride of 3 read index 3 as a key; because that
    // slot holds the string "type" it then read index 4, one past the end.
    await rejects(import('json', { with: { type: 'json', a: 'type' } }), {
      message: /^Unsupported import attribute: "a"/,
    });

    // Attribute order is the object's own-property order and is not sorted, so the
    // unsupported key must be reported regardless of which position it occupies.
    await rejects(import('json', { with: { a: 'type', type: 'json' } }), {
      message: /^Unsupported import attribute: "a"/,
    });

    // An unknown attribute is rejected during parsing, so it takes precedence over
    // the content-type mismatch that 'foo' would otherwise produce for type: 'json'.
    await rejects(import('foo', { with: { type: 'json', a: 'b' } }), {
      message: /^Unsupported import attribute: "a"/,
    });
  },
};

// Note: 'zebra: ...' parses as a URL (scheme "zebra" with an opaque path), so it
// resolves successfully and then fails as *not found*. This is deliberately the
// not-found path, NOT the invalid-specifier path (covered by
// `invalidModuleSpecifier` below).
export const invalidUrlAsSpecifier = {
  async test() {
    await rejects(import('zebra: not a \x00 valid URL'), {
      message: /Module not found/,
    });
  },
};

// The error class for a given module-resolution failure should be consistent
// regardless of which path (static-import, dynamic-import, or require())
// detects it:
//   * "Module not found"          -> Error     (a lookup failure, not a type error)
//   * "Invalid module specifier"  -> TypeError (a malformed value); see
//                                    `invalidModuleSpecifier`
//   * "Circular dependency ..."   -> Error     (a module-graph/loading error)
// See the resolve/dynamicResolve/require paths in jsg/modules-new.c++. The exact
// "Circular dependency when resolving module" message+class is pinned by the C++
// test in jsg/modules-new-test.c++; at the JS boundary it surfaces as a (still
// Error-classed) "Failed to instantiate module".
export const errorClassConsistency = {
  async test() {
    const myRequire = createRequire(import.meta.url);

    const assertPlainError = (err, label) => {
      ok(err instanceof Error, `${label}: expected an Error, got ${err}`);
      strictEqual(
        Object.getPrototypeOf(err),
        Error.prototype,
        `${label}: expected a plain Error, not a subclass like TypeError`
      );
    };

    // "Module not found" is a plain Error on every path.

    // Dynamic import:
    const dynNotFound = await import('module-not-found').then(
      () => null,
      (e) => e
    );
    assertPlainError(dynNotFound, 'dynamic import not-found');
    ok(/Module not found/.test(dynNotFound.message), dynNotFound.message);

    // Static import: importing a module whose own static import is missing
    // surfaces the static-import resolution failure.
    const staticNotFound = await import('static-import-missing').then(
      () => null,
      (e) => e
    );
    assertPlainError(staticNotFound, 'static import not-found');
    ok(/Module not found/.test(staticNotFound.message), staticNotFound.message);

    // require():
    let reqNotFound = null;
    try {
      myRequire('module-not-found');
    } catch (e) {
      reqNotFound = e;
    }
    assertPlainError(reqNotFound, 'require not-found');
    ok(/Module not found/.test(reqNotFound.message), reqNotFound.message);

    // A circular dependency is also a plain Error (not a TypeError).
    const circular = await import('circular-a').then(
      () => null,
      (e) => e
    );
    assertPlainError(circular, 'circular dependency');
  },
};

// A malformed/unparseable specifier is a TypeError on the dynamic-import,
// static-import and require() paths alike (matching Node's
// ERR_INVALID_MODULE_SPECIFIER, which extends TypeError). 'https://' is a
// special-scheme URL with no host, so it fails to parse rather than resolving to a
// (missing) module.
export const invalidModuleSpecifier = {
  async test() {
    // Dynamic import:
    const dyn = await import('https://').then(
      () => null,
      (e) => e
    );
    ok(dyn instanceof TypeError, `expected TypeError, got ${dyn && dyn.name}`);
    ok(/Invalid module specifier/.test(dyn.message), dyn.message);

    // Static import: a bundle ESM that statically imports the unparseable
    // specifier surfaces the same TypeError.
    const stat = await import('static-invalid-spec').then(
      () => null,
      (e) => e
    );
    ok(
      stat instanceof TypeError,
      `expected TypeError, got ${stat && stat.name}`
    );
    ok(/Invalid module specifier/.test(stat.message), stat.message);

    // require(): every one of these fails to parse as a URL, whether because the
    // special scheme has no host or because the host itself is malformed.
    for (const specifier of [
      'https://',
      'http://',
      'http://[',
      'https://[::1',
    ]) {
      throws(
        () => myRequire(specifier),
        (err) =>
          err instanceof TypeError &&
          /Invalid module specifier/.test(err.message),
        specifier
      );
    }
  },
};

export const dynamicImportAttributes = {
  async test() {
    const withoutAttributes = await import('json');
    const withAttributes = await import('json', {
      with: { type: 'json' },
    });

    deepStrictEqual(withAttributes.default, { foo: 1 });

    // Module::evaluateContext() currently ignores attributes when looking up the
    // per-isolate cache entry. Updating that TODO requires deliberately changing
    // this assertion along with the cache-key behavior.
    strictEqual(withAttributes, withoutAttributes);

    await rejects(import('text', { with: { type: 'text' } }), {
      name: 'TypeError',
      message: 'Import attribute type "text" is not yet supported',
    });
    await rejects(import('data', { with: { type: 'bytes' } }), {
      name: 'TypeError',
      message: 'Import attribute type "bytes" is not yet supported',
    });

    // A supported type that does not match the module's content type is rejected.
    await rejects(import('foo', { with: { type: 'json' } }), {
      message: /is not of type "json"/,
    });
  },
};

// A module specifier whose URL has an opaque path has a pathname with no leading "/":
// "opaque:foo" has the pathname "foo" and "opaque:" has an empty pathname. __filename and
// __dirname convert the pathname to a relative kj::Path, which must not consume a character
// that isn't a leading "/".
export const commonJsOpaquePathSpecifier = {
  async test() {
    const foo = await import('opaque:foo');
    strictEqual(foo.default.filename(), '/foo');
    strictEqual(foo.default.dirname(), '/');

    // An empty opaque path has neither a filename nor a directory.
    const empty = await import('opaque:');
    throws(() => empty.default.filename());
    throws(() => empty.default.dirname());
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

// Verify import.meta.filename and import.meta.dirname (WinterCG path helpers)
// work correctly for modules in subdirectories and are absent for non-file URLs.
export const importMetaPathHelpers = {
  async test() {
    // Module in a subdirectory should have correct dirname/filename.
    const { filename, dirname } = await import('sub/dir/meta-test');
    strictEqual(filename, '/bundle/sub/dir/meta-test');
    strictEqual(dirname, '/bundle/sub/dir');

    // Non-file URL modules should not have filename/dirname.
    const mod = await import('https://example.com/mod');
    strictEqual(mod.default, 'example');
    strictEqual(mod.filename, undefined);
    strictEqual(mod.dirname, undefined);
  },
};

// Regression test: getBuiltinModule called from a function defined in eval'd
// code should work. Previously failed on first invocation with "top-level await"
// error due to extra microtask tick from wrapSimplePromise().
export const getBuiltinModuleFromEval = {
  async test(_, env) {
    const code = `"use strict";async (exports)=>{
      exports.getPath = () => process.getBuiltinModule("node:path");
    }`;
    const exports = {};
    const fn = env.unsafe.eval(code);
    await fn(exports);

    const path = exports.getPath();
    ok(path.join, 'node:path should have join');
  },
};

// Regression test: createRequire called from a function defined in eval'd
// code should work. Same root cause as getBuiltinModuleFromEval.
export const createRequireFromEval = {
  async test(_, env) {
    const code = `"use strict";async (exports)=>{
      const { createRequire } = process.getBuiltinModule("node:module");
      const require = createRequire("/");
      exports.getUtil = () => require("node:util");
    }`;
    const exports = {};
    const fn = env.unsafe.eval(code);
    await fn(exports);

    const util = exports.getUtil();
    ok(util.promisify, 'node:util should have promisify');
  },
};

// Regression test: `node:process` (and bare `process`) is redirected to an
// internal module (node-internal:public_process / legacy_process) that only
// resolves in the builtin bucket. The static-import and require() paths force a
// BUILTIN_ONLY resolve context for the redirect, but the dynamic-import path
// previously derived its resolve context type from the *referrer* (a bundle
// module), so `await import('node:process')` failed with
// "Module not found: node-internal:public_process". This pins all three routes
// to the same instance so the dynamic path can't silently regress again.
// See maybeRedirectNodeProcess + dynamicResolve in jsg/modules-new.c++.
export const processRedirectAcrossResolutionRoutes = {
  async test() {
    const myRequire = createRequire(import.meta.url);

    // Static import (the branch that already worked).
    ok(processStatic, 'node:process default export should exist');
    strictEqual(typeof processStatic.nextTick, 'function');

    // Dynamic import, both prefixed and bare. This is the branch that used to
    // throw "Module not found: node-internal:public_process".
    const viaNode = await import('node:process');
    const viaBare = await import('process');
    strictEqual(typeof viaNode.default.nextTick, 'function');

    // Every route redirects to the same single internal process instance.
    strictEqual(viaNode.default, viaBare.default);
    strictEqual(viaNode.default, processStatic);
    strictEqual(myRequire('node:process'), processStatic);
  },
};

// Regression test: the node:process redirect ignores any query/fragment, just
// like every other built-in (which is resolved with IGNORE_SEARCH |
// IGNORE_FRAGMENTS). maybeRedirectNodeProcess previously matched the full href
// exactly, so `import('node:process?foo')` failed with
// "Module not found: node:process?foo" while e.g. `import('node:assert?foo')`
// resolved. See maybeRedirectNodeProcess in jsg/modules-new.c++.
export const processRedirectIgnoresQueryAndFragment = {
  async test() {
    const withQuery = await import('node:process?foo=1');
    const withFragment = await import('node:process#bar');
    strictEqual(typeof withQuery.default.nextTick, 'function');
    strictEqual(withQuery.default, processStatic);
    strictEqual(withFragment.default, processStatic);
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
// * [x] Import attributes are validated for static and dynamic imports
//   * [x] type: "json" succeeds for JSON modules
//   * [x] type: "text" and type: "bytes" report that they are not yet supported
//   * [x] Unsupported attribute keys are rejected
// * [x] require(...) Works in CommonJs Modules
// * [x] require(...) correctly handles node: modules with/without the node: prefix
// * [x] Circular dependencies are correctly handled
// * [x] Errors during CommonJs evaluation are correctly reported
// * [x] CommonJs modules correctly expose named exports
// * [x] require('module').createRequire API works as expected
// * [x] Entry point ESM with no default export is correctly reported as error
// * [x] Fallback service works as expected (server fallback-service tests)
// * [x] console.log output correctly uses node-internal:inspect for output
// ...
