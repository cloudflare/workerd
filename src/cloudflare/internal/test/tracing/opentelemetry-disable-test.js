// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { context, ROOT_CONTEXT, createContextKey } from '@opentelemetry/api';

export const disable = {
  test() {
    const key = createContextKey('disable');
    const value = context.active().setValue(key, 'value');
    context.with(value, () => {
      assert.strictEqual(context.active().getValue(key), 'value');
      context.disable();
      assert.strictEqual(context.active(), ROOT_CONTEXT);
    });
    assert.strictEqual(context.active(), ROOT_CONTEXT);
    assert.strictEqual(
      context.with(value, () => context.active()),
      ROOT_CONTEXT
    );
  },
};
