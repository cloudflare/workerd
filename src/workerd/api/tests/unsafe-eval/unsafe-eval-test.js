// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { doEval } from 'test:module';

export const test_can_use_eval_via_proxy = {
  async test() {
    assert.equal(doEval(), 2);
  },
};

// internal modules can't be imported
export const test_cannot_import_unsafe_eval = {
  async test() {
    await assert.rejects(import('internal:unsafe-eval'));
  },
};
