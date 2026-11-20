// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async test(ctrl, env) {
    const handled = await env.HANDLER.scheduled({ cron: '* * * * *' });
    if (handled.outcome !== 'ok') throw new Error(JSON.stringify(handled));
    const unhandled = await env.NO_HANDLER.scheduled({ cron: '* * * * *' });
    if (unhandled.outcome !== 'exception' || !unhandled.noRetry) {
      throw new Error(JSON.stringify(unhandled));
    }
  },
};
