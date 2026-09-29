// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject } from 'cloudflare:workers';

export class ContainerActor extends DurableObject {
  async fetch() {
    this.ctx.container.start();
    await this.ctx.container.setInactivityTimeout(60_000);
    return new Response('started');
  }
}

export default {
  fetch(_request, env) {
    return env.CONTAINER.getByName('graceful-shutdown').fetch(
      'http://container'
    );
  },
};
