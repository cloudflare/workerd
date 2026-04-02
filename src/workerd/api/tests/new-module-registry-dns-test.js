// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Test that the node:dns module works correctly with the new module registry.
// This exercises the Rust-implemented node-internal:dns module registration
// path, which uses an adapter to bridge the Rust ModuleCallback into the
// new BuiltinBuilder::addSynthetic API.

import { ok } from 'node:assert';
import dns from 'node:dns';
import dnsPromises from 'node:dns/promises';

export const basicDnsApi = {
  test() {
    // getServers() is a synchronous function that returns the list of
    // configured DNS servers. It exercises the node-internal:dns module.
    const servers = dns.getServers();
    ok(Array.isArray(servers));
  },
};

export const dnsPromisesApi = {
  test() {
    const resolver = new dnsPromises.Resolver();
    ok(resolver);
    const servers = resolver.getServers();
    ok(Array.isArray(servers));
  },
};
