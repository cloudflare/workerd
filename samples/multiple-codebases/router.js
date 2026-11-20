// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  fetch(request, env) {
    switch (new URL(request.url).hostname) {
      case 'one.example.test':
        return env.APP_ONE.fetch(request);
      case 'two.example.test':
        return env.APP_TWO.fetch(request);
      default:
        return new Response('Unknown host\n', { status: 404 });
    }
  },
};
