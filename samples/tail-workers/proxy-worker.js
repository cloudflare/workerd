// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

export default class RPCProxyWorker extends WorkerEntrypoint {

  async fetch(request) {
    console.log('Fetch event received in Proxy Worker');
    // Test if we can call tail handler manually
    await this.env.SERVICE.tail(['Proxy worker calling tail handler of tail-worker']);
    return this.env.SERVICE.fetch(request);
  }

  async tail(events) {
    console.log('Tail event received in Proxy Worker:', events);
    await this.env.SERVICE.tail('hello');
    await Promise.all([
      this.env.SERVICE.tail(events),
      this.env.SERVICE.fetch('http://example.com'),
    ]);
  }
}
