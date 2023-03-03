// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
/* eslint-disable */

// TODO(soon): Remove this once assert is out of experimental
import { default as CompatibilityFlags } from 'workerd:compatibility-flags';
if (!CompatibilityFlags.workerdExperimental) {
  throw new Error('node:stream is experimental.');
}

import { Writable } from 'node-internal:streams_writable';
export { Writable };
export default Writable;
