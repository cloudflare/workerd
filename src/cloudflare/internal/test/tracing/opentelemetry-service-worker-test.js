// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The new registry provides built-ins to service-worker scripts; the legacy registry does not.
function check() {
  const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
  if (Boolean(api) !== globalThis.expectedRegistration)
    throw new Error('Unexpected registration');
  if (api) {
    const span = api.trace.getTracer('service-worker').startSpan('noop');
    if (span.isRecording()) throw new Error('Unexpected recording');
    span.end();
  }
}
for (let i = 0; i < 3; i++) {
  check();
}
addEventListener('fetch', (event) => {
  check();
  event.respondWith(new Response('ok'));
});
