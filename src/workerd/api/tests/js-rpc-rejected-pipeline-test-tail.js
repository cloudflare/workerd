// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';

// Outcome and exception messages of each callee invocation, keyed by RPC
// method name. Events without an rpcMethod are ignored.
const outcomes = new Map();

export class Results extends WorkerEntrypoint {
  outcome(method) {
    return outcomes.get(method);
  }
}

export default {
  tail(events) {
    for (const event of events) {
      const method = event.event?.rpcMethod;
      if (method !== undefined) {
        outcomes.set(method, {
          outcome: event.outcome,
          exceptions: event.exceptions.map((e) => e.message),
        });
      }
    }
  },
};
