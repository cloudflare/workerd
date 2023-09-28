// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

addEventListener('fetch', event => {
  event.respondWith(handle(event.request));
});

async function foo() {
  // Only try to import node:net if not in a Workers environment
  if (navigator.userAgent !== 'Cloudflare-Workers') {
      // This should never be reached or evaluated:
      const net = await import('node:net'); // Or const net = require('node:net');
  }
  // else...
}

async function handle(request) {
  await foo();
  return new Response("Hello World\n");
}
