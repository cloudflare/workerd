// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the nodetach legacy window cell.

export {
  nodetachJsBackedStillDetaches,
  nodetachNativeKeepsCallerBuffer,
  nodetachInMemoryBodyDetaches,
} from 'legacy-nodetach';

// Serves the SELF binding: answers with the request's body.
export default {
  fetch(request) {
    return new Response(request.body);
  },
};
