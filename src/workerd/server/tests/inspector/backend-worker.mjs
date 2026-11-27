// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The target of debugger-worker.mjs's subrequest, which the Network panel reports.
export default {
  fetch() {
    return new Response(JSON.stringify({ hello: 'backend' }), {
      headers: { 'content-type': 'application/json', 'x-backend': 'yes' },
    });
  },
};
