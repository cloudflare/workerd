// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { DurableObject } from 'cloudflare:workers';

export default {
  async fetch(request, env) {
    return env.TARGET.fetch(`http://target${new URL(request.url).pathname}`);
  },
};

// Reusing the stub must not reuse a previous request's parent span.
export class Holder extends DurableObject {
  #target;

  async fetch(request) {
    this.#target ??= this.env.TARGET_OBJECT.get(
      this.env.TARGET_OBJECT.idFromName('target')
    );
    return this.#target.fetch(
      `http://target-object${new URL(request.url).pathname}`
    );
  }
}
