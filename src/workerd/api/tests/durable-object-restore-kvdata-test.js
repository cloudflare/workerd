// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

// workerd's storage back-ends implement neither snapshots nor point-in-time recovery, so a restore
// that passes validation fails with the back-end's error. The kvData checks run before that.
const BACK_END_ERROR =
  "This Durable Object's storage back-end does not implement point-in-time recovery.";

export class RestoreKvDataObject extends DurableObject {
  async restore(kvData) {
    try {
      await this.ctx.storage.onNextSessionRestore('bookmark', { kvData });
      return 'no error, unexpected';
    } catch (e) {
      return `${e.name}: ${e.message}`;
    }
  }
}

function kvDataWithKeys(count) {
  return Object.fromEntries(
    Array.from({ length: count }, (_, i) => [`key${i}`, i])
  );
}

export const kvDataLimits = {
  async test(ctrl, env) {
    const stub = env.ns.getByName('limits');

    assert.strictEqual(
      await stub.restore(kvDataWithKeys(21)),
      'RangeError: kvData can contain at most 20 keys.'
    );
    assert.strictEqual(
      await stub.restore({ big: 'x'.repeat(3 * 1024) }),
      'RangeError: kvData can contain at most 3072 bytes of keys and serialized values.'
    );
  },
};

export const kvDataRequiresSnapshot = {
  async test(ctrl, env) {
    const stub = env.ns.getByName('bookmark');

    // Within the limits, kvData is still rejected for a bookmark target...
    assert.strictEqual(
      await stub.restore(kvDataWithKeys(20)),
      'TypeError: kvData is only supported when restoring from a snapshot.'
    );

    // ... but a bookmark restore without kvData reaches the back-end.
    assert.strictEqual(await stub.restore(undefined), `Error: ${BACK_END_ERROR}`);
    assert.strictEqual(await stub.restore({}), `Error: ${BACK_END_ERROR}`);
  },
};
