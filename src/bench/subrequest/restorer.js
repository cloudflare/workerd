// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Mints a stub of `TARGET` restored through `ctx.restore()` for subrequest.js. It reaches the
// caller over RPC as a restored channel token, which the channel token handler decodes into a C++
// restored channel.

import { WorkerEntrypoint, restore } from 'cloudflare:workers';

export class Restorer extends WorkerEntrypoint {
  stub() {
    return this.ctx.restore({});
  }

  [restore]() {
    return this.env.TARGET;
  }
}
