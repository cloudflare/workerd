// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The main module of a bundle in which the launcher is expected to fail: it evaluates the
// launcher at startup, as a Wasm-only worker would, and reports how that went.
const outcome = await import('./launcher.js').then(
  () => ({ loaded: true }),
  (e) => ({ name: e.name, message: e.message, hasCause: e.cause !== undefined })
);

export default {
  fetch() {
    return Response.json(outcome);
  },
};
