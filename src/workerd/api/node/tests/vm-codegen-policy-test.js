// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import * as vm from 'node:vm';

export const vmDoesNotGrantCodeGeneration = {
  test() {
    // Enabling VM support must not turn request-time code generation on.
    assert.throws(() => new vm.Script('1 + 1'), { name: 'EvalError' });
    assert.throws(() => vm.compileFunction('return 1'), { name: 'EvalError' });
    assert.throws(() => new vm.SourceTextModule('export const value = 1'), {
      name: 'EvalError',
    });
    assert.throws(
      () =>
        vm.runInNewContext(
          '1 + 1',
          {},
          {
            contextCodeGeneration: { strings: true, wasm: true },
          }
        ),
      { name: 'EvalError' }
    );
  },
};
