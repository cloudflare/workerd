// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Service Workers syntax "scheduled" listener that does what the event's cron pattern names.
addEventListener('scheduled', (event) => {
  console.log(`listener ${event.cron} ${event.scheduledTime}`);
  switch (event.cron) {
    case 'no-retry':
      event.noRetry();
      break;
    case 'throw':
      throw new Error('thrown by the listener');
    case 'wait-until':
      event.waitUntil(
        scheduler.wait(10).then(() => console.log('waitUntil finished'))
      );
      break;
  }
});
