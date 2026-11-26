// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Service Workers syntax Worker with no "scheduled" listener.
addEventListener('fetch', (event) => {
  event.respondWith(new Response('ok'));
});
