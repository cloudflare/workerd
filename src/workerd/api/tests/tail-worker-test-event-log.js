// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A buffered tail worker that logs the event of each trace it receives, for its own streaming
// tail worker to check.
export default {
  tail(traces) {
    console.log(JSON.stringify(traces.map((trace) => trace.event)));
  },
};
