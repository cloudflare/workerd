// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Reading the lazy global in service-worker syntax must neither throw nor poison later calls.
for (let i = 0; i < 3; i++) {
  if (globalThis[Symbol.for('opentelemetry.js.api.1')] !== undefined) {
    throw new Error('Service-worker syntax must leave OTel unregistered');
  }
}
addEventListener('fetch', (event) => {
  if (globalThis[Symbol.for('opentelemetry.js.api.1')] !== undefined) {
    throw new Error('Unexpected registration');
  }
  event.respondWith(new Response('ok'));
});
