// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests the `hasSubscribers` getter behavior of `Channel` and `TracingChannel`
// when the `diagnostics_channel_has_subscribers_getter` compat flag is enabled.
// Legacy method behavior is covered by `diagnostics-channel-test`.

import { ok, strictEqual } from 'node:assert';

import { channel, tracingChannel } from 'node:diagnostics_channel';

export const test_channel_hasSubscribers_is_a_getter = {
  async test() {
    const ch = channel('getter-test');

    // It is a boolean, not a function (matches Node.js).
    strictEqual(typeof ch.hasSubscribers, 'boolean');
    strictEqual(ch.hasSubscribers, false);

    const listener = () => {};
    ch.subscribe(listener);
    strictEqual(ch.hasSubscribers, true);

    ch.unsubscribe(listener);
    strictEqual(ch.hasSubscribers, false);

    // Defined on the prototype as a read-only getter.
    const desc = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(ch),
      'hasSubscribers'
    );
    ok(desc);
    strictEqual(typeof desc.get, 'function');
    strictEqual(desc.set, undefined);
    strictEqual(desc.value, undefined);
  },
};

export const test_tracingChannel_hasSubscribers_is_a_getter = {
  async test() {
    const tc = tracingChannel('tracing-getter-test');

    strictEqual(typeof tc.hasSubscribers, 'boolean');
    strictEqual(tc.hasSubscribers, false);

    const listener = () => {};

    // Each sub-channel independently flips the aggregate getter.
    for (const sub of ['start', 'end', 'asyncStart', 'asyncEnd', 'error']) {
      tc[sub].subscribe(listener);
      strictEqual(tc.hasSubscribers, true, `via ${sub}`);
      tc[sub].unsubscribe(listener);
      strictEqual(tc.hasSubscribers, false, `after unsubscribing ${sub}`);
    }

    // TracingChannel.hasSubscribers should also be a prototype getter.
    const desc = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(tc),
      'hasSubscribers'
    );
    ok(desc);
    strictEqual(typeof desc.get, 'function');
    strictEqual(desc.set, undefined);
    strictEqual(desc.value, undefined);
  },
};
