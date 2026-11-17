// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The case-cjs service of startup-snapshot-modules-test: the shared case plus a CommonJS module
// imported at the top level, whose default export is compared with a runtime import() of it.

import cjs from './cjs.js';
import { report } from './worker.js';

export default {
  async fetch() {
    const dynCjs = await import('./cjs.js');
    return Response.json(await report({ cjsDefault: dynCjs.default === cjs }));
  },
};
