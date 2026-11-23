// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A small async Worker: makes short-lived garbage and awaits a few promises that the runtime
// settles, as most real Workers do before they respond.
export default {
  async fetch() {
    const parts = [];
    for (let i = 0; i < 100; i++) parts.push({ i, s: 'x' + i });
    for (let i = 0; i < 4; i++) await new Response('chunk' + i).text();
    return new Response('hello world');
  },
};
