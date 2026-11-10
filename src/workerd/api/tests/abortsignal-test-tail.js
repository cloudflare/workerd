// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Records each callee invocation's outcome and exception messages by RPC method name.
import { WorkerEntrypoint } from 'cloudflare:workers';

const outcomes = new Map();

export class Results extends WorkerEntrypoint {
  outcome(method) {
    return outcomes.get(method);
  }
}

export default {
  tail(events) {
    for (const { event, outcome, exceptions } of events) {
      if (event?.rpcMethod !== undefined) {
        outcomes.set(event.rpcMethod, {
          outcome,
          exceptions: exceptions.map((e) => e.message),
        });
      }
    }
  },
};
