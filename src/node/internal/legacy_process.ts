// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// Legacy process wrapper only exports limited process exports.
// This is used when the enable_nodejs_process_v2 compat flag is disabled.
// node:process re-mapping for this flag is done via module resolution.
import { default as processImpl } from 'node-internal:process';

import {
  platform,
  nextTick,
  env,
  features,
  _setEventsProcess,
} from 'node-internal:internal_process';

export { platform, nextTick, env, features };

export function getBuiltinModule(id: string): object {
  return processImpl.getBuiltinModule(id);
}

export function exit(code: number): void {
  processImpl.exitImpl(code);
}

const process = {
  nextTick,
  env,
  exit,
  getBuiltinModule,
  platform,
  features,
};

_setEventsProcess(process);

export default process;
