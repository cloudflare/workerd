// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';
import { describe, makeError } from 'error-stacks-common';

function remoteMakesError(kind) {
  return makeError(kind, 'remote');
}

export default class extends WorkerEntrypoint {
  throwError(kind) {
    throw remoteMakesError(kind);
  }

  // The error is deep in the value.
  returnError(kind) {
    return { deep: [1, { error: remoteMakesError(kind) }] };
  }

  describeError(value) {
    return describe(value.deep[1].error);
  }
}
