// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The tail test checks that each target continues this caller's span through the untraced hop.

export default {
  async test(controller, env, ctx) {
    for (const path of ['/1', '/2']) {
      await env.RELAY.fetch(`http://relay${path}`);
    }

    // The holder keeps its stub to the target across these two requests.
    const holder = env.HOLDER.get(env.HOLDER.idFromName('holder'));
    for (const path of ['/1', '/2']) {
      await holder.fetch(`http://holder${path}`);
    }
  },
};
