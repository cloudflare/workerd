// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import rtti from 'workerd:rtti';

export default {
  fetch(request) {
    const [date, ...flags] = new URL(request.url).pathname
      .substring(1)
      .split('+');
    return new Response(rtti.exportTypes(date, flags));
  },
};
