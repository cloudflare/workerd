// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import module, { wrap as namedWrap } from 'node:module';
import { ok, deepStrictEqual, strictEqual, throws } from 'node:assert';

export const testUnimplemented = {
  async test() {
    const fields = [
      '_cache',
      '_pathCache',
      '_extensions',
      'globalPaths',
      '_debug',
      'isBuiltin',
      '_findPath',
      '_nodeModulePaths',
      '_resolveLookupPaths',
      '_load',
      '_resolveFilename',
      'createRequire',
      '_initPaths',
      '_preloadModules',
      'syncBuiltinESMExports',
      'Module',
      'registerHooks',
      'builtinModules',
      'runMain',
      'register',
      'constants',
      'enableCompileCache',
      'findPackageJSON',
      'flushCompileCache',
      'stripTypeScriptTypes',
      'getCompileCacheDir',
      'findSourceMap',
      'SourceMap',
      'getSourceMapsSupport',
      'setSourceMapsSupport',
    ];

    for (const field of fields) {
      ok(field in module, `${field} is not in module`);
      ok(module[field] != null, `${field} is ${typeof module[field]}`);
    }

    deepStrictEqual(Object.keys(module.Module.prototype), [
      'load',
      'require',
      '_compile',
    ]);
  },
};

export const testWrap = {
  async test() {
    const head =
      '(function (exports, require, module, __filename, __dirname) { ';
    const tail = '\n});';

    deepStrictEqual(module.wrapper, [head, tail]);
    strictEqual(module.wrap('"hello world"'), head + '"hello world"' + tail);
    strictEqual(module.wrap(), head + 'undefined' + tail);
    strictEqual(module.wrap(1), head + '1' + tail);
    // Objects convert with the default hint, so valueOf() wins.
    strictEqual(
      module.wrap({ valueOf: () => 'v', toString: () => 's' }),
      head + 'v' + tail
    );
    strictEqual(namedWrap, module.wrap);

    for (const name of ['wrap', 'wrapper']) {
      const desc = Object.getOwnPropertyDescriptor(module, name);
      strictEqual(typeof desc.get, 'function');
      strictEqual(typeof desc.set, 'function');
      strictEqual(desc.enumerable, false);
      strictEqual(desc.configurable, false);
    }

    const originalWrapper = module.wrapper;
    const originalWrap = module.wrap;
    try {
      module.wrapper[0] = 'A';
      strictEqual(module.wrap('x'), 'Ax' + tail);
      module.wrapper[0] = head;

      module.wrapper = ['B', 'C'];
      strictEqual(module.wrap('x'), 'BxC');
      strictEqual(namedWrap('x'), 'BxC');

      module.wrapper = null;
      throws(() => module.wrap('x'), TypeError);
      module.wrapper = originalWrapper;

      const replacement = () => 'Z';
      module.wrap = replacement;
      strictEqual(module.wrap, replacement);
      strictEqual(module.wrap('x'), 'Z');
      strictEqual(originalWrap('y'), head + 'y' + tail);
    } finally {
      module.wrapper = originalWrapper;
      module.wrap = originalWrap;
    }
    strictEqual(module.wrap('x'), head + 'x' + tail);
  },
};
