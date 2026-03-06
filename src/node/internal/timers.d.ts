// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type {
  setImmediate as setImmediateImpl,
  clearImmediate as clearImmediateImpl,
} from 'node:timers';

export const setImmediate: typeof setImmediateImpl;
export const clearImmediate: typeof clearImmediateImpl;
