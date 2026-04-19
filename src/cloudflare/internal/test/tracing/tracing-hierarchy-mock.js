// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Minimal fetch target used by tracing-hierarchy-test. Just echoes the request path so
// the runtime-generated "fetch" span has something to observe.
export default {
  async fetch(request) {
    return new Response('ok', { status: 200 });
  },
};
