// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';
import { describe, makeError } from 'error-stacks-common';

function remoteMakesError() {
  return makeError('remote');
}

export default class extends WorkerEntrypoint {
  throwError() {
    throw remoteMakesError();
  }

  // The error is deep in the value.
  returnError() {
    return { deep: [1, { error: remoteMakesError() }] };
  }

  describeError(value) {
    return describe(value.deep[1].error);
  }
}
