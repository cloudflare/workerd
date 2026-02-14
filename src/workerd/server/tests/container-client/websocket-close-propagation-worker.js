// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';
import { scheduler } from 'node:timers/promises';

export default {
  async fetch(request, env) {
    const upgrade = request.headers.get('Upgrade');
    if (upgrade?.toLowerCase() !== 'websocket') {
      return new Response('expected websocket', { status: 400 });
    }

    const id = env.MY_CONTAINER.idFromName('repro');
    const stub = env.MY_CONTAINER.get(id);
    return stub.fetch(request);
  },
};

export class ContainerProxy extends DurableObject {
  async fetch(request) {
    const { container } = this.ctx;

    if (!container.running) {
      container.start({
        env: { WS_ENABLED: 'true' },
        enableInternet: true,
      });
    }

    // Proxy the websocket upgrade into the container and return it to the
    // eyeball client. Close events should propagate back through the same
    // path as data.
    const maxRetries = 6;
    for (let i = 1; i <= maxRetries; i++) {
      try {
        return await container.getTcpPort(8080).fetch('http://container/ws', {
          headers: {
            Upgrade: 'websocket',
            Connection: 'Upgrade',
            'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
            'Sec-WebSocket-Version': '13',
          },
        });
      } catch (e) {
        if (!e.message.includes('container port not found')) {
          throw e;
        }
        console.info(
          `Retrying getTcpPort(8080) for the ${i} time due to an error ${e.message}`
        );
        console.info(e);
        if (i === maxRetries) {
          console.error(
            `Failed to connect to container for WebSocket. Retried ${i} times`
          );
          throw e;
        }
        await scheduler.wait(1000);
      }
    }
    throw new Error('unreachable');
  }
}
