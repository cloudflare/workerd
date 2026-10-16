// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Events beyond the events implementation: structured clone, inspection,
// and the runtime's own Event subclasses. Until the integration phases the
// TypeScript events are not platform objects the rest of the runtime
// recognizes, so each of these diverges (ledger rows marked †; how they
// cross targets is event-target/interop.js's).
//
// Divergences (ledger in AGENTS.md):
// - #17 †: structuredClone() of an event.
// - #18 †: util.inspect() of an event.
// - #19 †: the runtime's Event subclasses are C++ events.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import util from 'node:util';
import { usingTsImpl } from 'which-impl';

// #17 †. TS clones the event as an ordinary object with its one own
// property, read through the getter.
export const structuredCloneDiverges = {
  test() {
    const event = new Event('x');
    if (usingTsImpl) {
      const clone = structuredClone(event);
      deepStrictEqual(clone, { isTrusted: false });
      strictEqual(Object.getPrototypeOf(clone), Object.prototype);
    } else {
      throws(() => structuredClone(event), { name: 'DataCloneError' });
    }
  },
};

// #18 †. TS shows only the own isTrusted accessor.
export const inspectDiverges = {
  test() {
    const inspected = util.inspect(new Event('x'));
    if (usingTsImpl) {
      strictEqual(inspected, 'Event { isTrusted: [Getter] }');
    } else {
      ok(inspected.startsWith('Event {'), inspected);
      ok(inspected.includes("type: 'x'"), inspected);
      ok(inspected.includes('timeStamp: 0'), inspected);
    }
  },
};

// #19 †. MessageEvent, ErrorEvent, CloseEvent and the rest inherit from the
// C++ Event, which the TS Event's members reject.
export const runtimeEventsAreCppEvents = {
  test() {
    const { get } = Object.getOwnPropertyDescriptor(Event.prototype, 'type');
    for (const event of [
      new MessageEvent('message'),
      new ErrorEvent('error'),
      new CloseEvent('close'),
    ]) {
      strictEqual(event instanceof Event, !usingTsImpl, event.type);
      if (usingTsImpl) {
        throws(() => get.call(event), TypeError, event.type);
      } else {
        strictEqual(get.call(event), event.type);
      }
    }
  },
};
