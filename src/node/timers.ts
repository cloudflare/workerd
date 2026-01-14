// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  active,
  clearImmediate,
  clearInterval,
  clearTimeout,
  enroll,
  setImmediate,
  setInterval,
  setTimeout,
  unenroll,
} from 'node-internal:internal_timers'
import * as _promises from 'node-internal:internal_timers_promises'

export * from 'node-internal:internal_timers'
export const promises = _promises

export default {
  promises: _promises,
  setTimeout,
  clearTimeout,
  setImmediate,
  clearImmediate,
  setInterval,
  clearInterval,
  active,
  unenroll,
  enroll,
}
