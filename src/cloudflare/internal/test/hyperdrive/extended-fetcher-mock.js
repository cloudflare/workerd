// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import wrappedBinding from 'cloudflare-internal:wrapped-binding';

// Stands in for the ExtendedFetcher that production passes to `cloudflare-internal:hyperdrive`,
// which workerd's config cannot construct: a service binding that also exposes a random `host` and
// a fixed `port`. connect() forwards to the inner service binding's connect handler.
class MockExtendedFetcher extends wrappedBinding.WrappedBinding {
  #fetcher;
  #host;
  #port;

  constructor(env) {
    super(env.fetcher);
    this.#fetcher = env.fetcher;
    this.#port = env.port;
  }

  // Like the real ExtendedFetcher, the host is generated lazily (bindings are constructed at
  // global scope, where generating random values is disallowed) and has the same shape:
  // `<database>.<user>.<password>.hyperdrive.local`, three independent 32-hex labels.
  get host() {
    if (this.#host === undefined) {
      const label = () =>
        Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) =>
          b.toString(16).padStart(2, '0')
        ).join('');
      this.#host = `${label()}.${label()}.${label()}.hyperdrive.local`;
    }
    return this.#host;
  }

  get port() {
    return this.#port;
  }

  connect(address) {
    return this.#fetcher.connect(address);
  }
}

export default function makeMockExtendedFetcher(env) {
  return new MockExtendedFetcher(env);
}
