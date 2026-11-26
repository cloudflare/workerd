// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A scheduled() handler that does what the event's cron pattern names.
export default {
  scheduled(controller, env, ctx) {
    console.log(
      `scheduled ${controller.cron} ${controller.scheduledTime} ${typeof env} ${typeof ctx}`
    );
    switch (controller.cron) {
      case 'reject':
        return Promise.reject(new Error('rejected'));
      case 'wait-until':
        ctx.waitUntil(
          scheduler.wait(10).then(() => console.log('waitUntil finished'))
        );
        return;
      default:
        return;
    }
  },
};

// An entrypoint without a scheduled() handler.
export const noHandler = {
  fetch() {
    return new Response('ok');
  },
};
