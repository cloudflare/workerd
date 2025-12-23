// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This module overrides globalThis timer functions with Node.js-compatible versions
// when loaded. It is loaded by worker.c++ when enable_nodejs_global_timers compat
// flag is enabled.
//
// After loading:
// - globalThis.setTimeout and globalThis.setInterval return Timeout objects
//   with methods like refresh(), ref(), unref(), and hasRef() instead of numeric IDs
// - globalThis.setImmediate and globalThis.clearImmediate are available

import {
  setTimeout,
  setInterval,
  clearTimeout,
  clearInterval,
  setImmediate,
  clearImmediate,
} from 'node-internal:internal_timers';

globalThis.setTimeout = setTimeout as unknown as typeof globalThis.setTimeout;
globalThis.setInterval =
  setInterval as unknown as typeof globalThis.setInterval;
globalThis.clearTimeout =
  clearTimeout as unknown as typeof globalThis.clearTimeout;
globalThis.clearInterval =
  clearInterval as unknown as typeof globalThis.clearInterval;
globalThis.setImmediate = setImmediate;
globalThis.clearImmediate = clearImmediate;
