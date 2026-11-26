// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The service that subrequest.js sends its subrequests to: each does as little as it can.

import { WorkerEntrypoint } from 'cloudflare:workers';

export class Rpc extends WorkerEntrypoint {
  ping() {
    return 1;
  }
}

export default {
  fetch() {
    return new Response(null);
  },
};
