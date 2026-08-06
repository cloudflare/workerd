// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import wrappedBinding from 'cloudflare-internal:wrapped-binding';

// A minimal wrapped-binding wrapper module, modeled after real wrapped bindings (e.g. D1's
// `cloudflare-internal:d1-api`): the default export receives `env` containing the inner stub and
// returns the user-facing class instance.
//
class Door extends wrappedBinding.WrappedBinding {
  #fetcher;

  constructor(env) {
    super(env.fetcher);
    this.#fetcher = env.fetcher;
    this.self = this;
    // A property set by the wrapper each time it (re)instantiates. Verifies a deserialized
    // wrapped binding is reconstructed by re-running the wrapper, not by structurally cloning.
    this.label = 'wrapped-door';
  }

  async ping() {
    const resp = await this.#fetcher.fetch('http://placeholder/ping');
    return await resp.text();
  }
}

export default function makeDoor(env) {
  if (!env.fetcher) {
    throw new Error('inner "fetcher" binding is missing');
  }
  return new Door(env);
}
