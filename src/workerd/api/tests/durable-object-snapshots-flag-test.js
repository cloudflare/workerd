// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export class SnapshotsFlagObject {
  constructor(state) {
    this.storage = state.storage;
  }

  fetch() {
    return Response.json({
      snapshot: typeof this.storage.snapshot,
      onNextSessionRestore: typeof this.storage.onNextSessionRestore,
      waitForBookmark: typeof this.storage.waitForBookmark,
    });
  }
}

// Each worker in the config passes the storage members that its compatibility
// flags should expose. The durable_object_snapshots flag exposes the snapshot
// APIs without the members that still require the catch-all experimental flag.
export const storageSurfaceMatchesFlags = {
  async test(ctrl, env) {
    const stub = env.ns.get(env.ns.idFromName('test'));
    const response = await stub.fetch('http://example.test/');
    assert.deepStrictEqual(await response.json(), env.expectedSurface);
  },
};
