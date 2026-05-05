// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Minimal fetch + RPC target used by tracing-hierarchy-test.
import { WorkerEntrypoint } from 'cloudflare:workers';

export default {
  async fetch(request) {
    return new Response('ok', { status: 200 });
  },
};

export class RpcTarget extends WorkerEntrypoint {
  async ping() {
    return 'pong';
  }
}
