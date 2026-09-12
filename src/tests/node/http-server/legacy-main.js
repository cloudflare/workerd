// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the legacy (unflagged) node:http server cell. As in
// main.js, the default export routes a test's fetch to its server.

import { route } from 'harness';

export default {
  fetch(request, env, ctx) {
    return route(request, env, ctx);
  },
};

export {
  legacyFirstBodyWriteHitsConstructorGate,
  legacyUncaughtGateErrorFailsFetch,
  legacyBodilessResponsesWork,
  legacyRequestBodyIsPumped,
} from 'legacy-constructor-gate';
