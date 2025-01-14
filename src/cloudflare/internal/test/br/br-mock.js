// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request, env, ctx) {
    return Response.json(
      { success: true },
      {
        headers: {
          'content-type': 'application/json',
        },
      }
    );
  },
};
