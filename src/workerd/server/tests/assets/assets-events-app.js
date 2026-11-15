// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The user Worker behind an assets service. Each event it gets settles a promise the test awaits
// over RPC.
import { WorkerEntrypoint } from 'cloudflare:workers';

let onScheduled, onTail;
const scheduled = new Promise((resolve) => (onScheduled = resolve));
const tailed = new Promise((resolve) => (onTail = resolve));

export default class extends WorkerEntrypoint {
  fetch() {
    return new Response('from the user Worker');
  }

  scheduled(controller) {
    onScheduled(controller.cron);
  }

  tail(events) {
    onTail(events.flatMap((event) => event.logs.map((log) => log.message[0])));
  }

  hello(name) {
    return `hello, ${name}`;
  }

  scheduledCron() {
    return scheduled;
  }

  tailedLogs() {
    return tailed;
  }
}
