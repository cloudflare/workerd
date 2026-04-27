// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';

const results = [];

export class Receiver extends WorkerEntrypoint {
  async capture(record) {
    results.push(record);
    return { ok: true };
  }

  async reset() {
    results.length = 0;
  }

  async getResults() {
    return results.slice();
  }
}
