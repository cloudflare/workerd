// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  fetch(request, env) {
    console.log('Fetch event received in My Worker', request.method, request.url);
    return env.SERVICE.fetch(request);
  },
};
