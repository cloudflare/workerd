// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject } from 'cloudflare:workers';

export class ContainerActor extends DurableObject {
  async fetch(request) {
    const container = this.ctx.container;
    if (!container.running) {
      container.start();
    }
    // Container calls run in order, so this resolves once the container has started.
    await container.setInactivityTimeout(60_000);

    if (new URL(request.url).pathname === '/restore') {
      const snapshot = await container.snapshotDirectory({ dir: '/tmp' });
      await container.destroy();
      container.start({ directorySnapshots: [{ snapshot }] });
      await container.setInactivityTimeout(60_000);
      return Response.json(snapshot);
    }
    return new Response('started');
  }
}

export default {
  fetch(request, env) {
    return env.CONTAINER.getByName('session').fetch(request);
  },
};
