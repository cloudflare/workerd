// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export default {
  fetch(request) {
    const params = new URL(request.url).searchParams;
    const init = { status: Number(params.get('status')) };
    if (params.has('statusText')) {
      init.statusText = params.get('statusText');
    }
    return new Response(null, init);
  },
};
