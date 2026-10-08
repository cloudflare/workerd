// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

export default class extends WorkerEntrypoint {
  async fetch(request) {
    return this.env.TARGET.fetch(
      `http://target${new URL(request.url).pathname}`
    );
  }

  async forward(path) {
    const holder = this.env.HOLDER.get(this.env.HOLDER.idFromName('holder'));
    return holder.forward(path);
  }
}

// Reusing the stub must not reuse a previous request's parent span.
export class Holder extends DurableObject {
  #target;

  async forward(path) {
    const response = await this.fetch(new Request(`http://holder${path}`));
    return response.text();
  }

  async fetch(request) {
    this.#target ??= this.env.TARGET_OBJECT.get(
      this.env.TARGET_OBJECT.idFromName('target')
    );
    return this.#target.fetch(
      `http://target-object${new URL(request.url).pathname}`
    );
  }
}
