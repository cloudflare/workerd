// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Verifies that the `no_temporal` compatibility flag removes the Temporal API.
//
// V8 installs Temporal on every context, because --harmony-temporal is
// process-wide and cannot vary per isolate. Gating it per Worker therefore
// means deleting what V8 already installed, so the absence asserted here is
// the result of jsg::deleteTemporalGlobals() running at context creation
// rather than of V8 declining to install anything.

import { strictEqual } from 'node:assert';

export const temporalGlobalIsAbsent = {
  test() {
    // `typeof` on an undeclared identifier evaluates to "undefined" rather
    // than throwing, which is exactly the check applications use to decide
    // whether to install a polyfill. It is the property that matters to them,
    // so it is the property asserted here.
    strictEqual(typeof Temporal, 'undefined');

    // V8 installs the global as a lazy accessor with replace-on-access, so it
    // would still be present as a property even if nothing had read it. Check
    // for the property too, not just for a successful read.
    strictEqual('Temporal' in globalThis, false);
    strictEqual(Object.hasOwn(globalThis, 'Temporal'), false);
  },
};

export const dateBridgeIsAbsent = {
  test() {
    // Genesis::InitializeGlobal_harmony_temporal() installs
    // Date.prototype.toTemporalInstant alongside the global, so it is a second
    // route into the API and has to be absent as well.
    strictEqual(typeof Date.prototype.toTemporalInstant, 'undefined');
    strictEqual('toTemporalInstant' in Date.prototype, false);

    // Date itself must be otherwise untouched by the deletion.
    strictEqual(new Date(0).getTime(), 0);
    strictEqual(typeof Date.now(), 'number');
  },
};
