// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { onHibernate } from 'cloudflare:durable-objects';

export const symbolRequiresExperimental = {
  test() {
    strictEqual(onHibernate, undefined);
  },
};
