// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';
import {
  checkInspect,
  checkMapVirtualHost,
  checkRestoreErrors,
  checkShape,
} from 'ctx-shape';

export class Shape extends WorkerEntrypoint {
  check() {
    checkShape(this.ctx);
  }
  checkFlagged() {
    checkInspect(this.ctx);
    checkMapVirtualHost(this.ctx, this.env.SELF);
  }
  async checkRestoreErrors() {
    await checkRestoreErrors(this.ctx);
  }
}
