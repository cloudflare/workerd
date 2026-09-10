// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { createRequire, isBuiltin, builtinModules } from 'node:module';
import { ok, strictEqual, throws } from 'node:assert';

export const doTheTest = {
  async test() {
    // Under the new module registry, module specifiers are URLs and worker
    // bundle modules live under file:///bundle/, so a require rooted at '/'
    // (i.e. file:///) cannot see them; root the require at the bundle instead.
    // Under the original registry, module names are bare paths and '/' works.
    const isNMR = Cloudflare.compatibilityFlags.new_module_registry;
    const require = createRequire(isNMR ? 'file:///bundle/' : '/');
    ok(typeof require === 'function');

    const foo = require('foo');
    const bar = require('bar');
    const baz = require('baz');
    const qux = require('worker/qux');

    // The new module registry follows Node.js require(esm) semantics: a plain
    // ESM yields its namespace. The original registry consults the
    // require_returns_default_export flag (which does not apply to the new
    // registry).
    if (
      !isNMR &&
      Cloudflare.compatibilityFlags.require_returns_default_export
    ) {
      strictEqual(foo, 1);
    } else {
      strictEqual(foo.default, 1);
    }
    strictEqual(bar, 2);
    strictEqual(baz, 3);
    strictEqual(qux, '4');

    const assert = await import('node:assert');
    const required = require('node:assert');

    // Builtin ESM wraps a CJS-style API in its default export; the new module
    // registry and the require_returns_default_export flag both unwrap it.
    if (isNMR || Cloudflare.compatibilityFlags.require_returns_default_export) {
      strictEqual(assert.default, required);
    } else {
      strictEqual(assert, required);
    }

    // Top-level await is never permitted in require()d modules. The error
    // message differs between the registries.
    const tlaError = isNMR
      ? { message: /^Top-level await is not supported in this context/ }
      : { message: 'Top-level await in module is not permitted at this time.' };
    throws(() => require('invalid'), tlaError);
    // Trying to require the module again should throw the same error.
    throws(() => require('invalid'), tlaError);
    throws(() => require('invalid2'), tlaError);

    throws(() => require('does not exist'));
    throws(() => createRequire('not a valid path'), {
      message: /The argument must be a file URL object/,
    });
    throws(() => createRequire(new URL('http://example.org')), {
      message: /The argument must be a file URL object/,
    });

    // The new module registry handles specifiers as URLs, so query strings and
    // hash fragments on the referrer are permitted there; the original
    // registry rejects them.
    if (isNMR) {
      createRequire('file://test?abc');
      createRequire('file://test#123');
    } else {
      throws(() => createRequire('file://test?abc'), {
        message:
          'The specifier must not have query string parameters or hash fragments.',
      });
      throws(() => createRequire('file://test#123'), {
        message:
          'The specifier must not have query string parameters or hash fragments.',
      });
    }

    // These should not throw...
    createRequire('file:///');
    createRequire('file:///tmp');
    createRequire(new URL('file:///'));
  },
};

export const isBuiltinTest = {
  test() {
    ok(isBuiltin('fs'));
    ok(isBuiltin('http'));
    ok(isBuiltin('https'));
    ok(isBuiltin('path'));
    ok(isBuiltin('node:fs'));
    ok(isBuiltin('node:http'));
    ok(isBuiltin('node:https'));
    ok(isBuiltin('node:path'));
    ok(isBuiltin('node:test'));
    ok(!isBuiltin('test'));
    ok(!isBuiltin('worker'));
    ok(!isBuiltin('worker/qux'));

    builtinModules.forEach((module) => {
      ok(isBuiltin(module));
    });
  },
};
