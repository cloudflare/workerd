// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';
import module from 'compact-imports';

export const wasmCompactImportsTest = {
  async test() {
    deepStrictEqual(WebAssembly.Module.imports(module), [
      { module: 'a', name: 'f', kind: 'function' },
      { module: 'a', name: 'g', kind: 'global' },
      { module: 'b', name: 'x', kind: 'function' },
      { module: 'b', name: 'y', kind: 'function' },
    ]);
    const instance = await WebAssembly.instantiate(module, {
      a: { f: (v) => v + 1, g: 1000 },
      b: { x: (v) => v * 2, y: (v) => v * 3 },
    });
    strictEqual(instance.exports.run(1), 1007);
  },
};
