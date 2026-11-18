// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The toy's hello, as a JavaScript Worker.
export default {
  fetch() {
    return new Response('Hello, world!', {
      headers: { 'Content-Type': 'text/plain' },
    });
  },
};
