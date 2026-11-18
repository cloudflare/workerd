// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Forwards every request to the `backend` binding and returns its response.
export default {
  fetch(request, env) {
    return env.backend.fetch(request);
  },
};
