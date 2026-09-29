// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Hyperdrive, type HyperdriveEnv } from 'cloudflare-internal:hyperdrive';

export default function makeBinding(env: HyperdriveEnv): Hyperdrive {
  return new Hyperdrive(env, 'mysql');
}
