// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without workers_api_getters_setters_on_prototype, Event's attributes are
// properties of each instance, which V8 reports as data properties. None is
// on the prototype, so a subclass cannot override one, yet every read still
// runs the getter and assigning cancelBubble still runs its setter.
// (CustomEvent's detail is a prototype accessor either way.) srcElement
// reads the current target, not the target: outside dispatch it is null
// where target is undefined. The event-target suite's legacy cell has it
// during and after a dispatch.

import { deepStrictEqual, strictEqual } from 'node:assert';

// Sorted.
const kAttributes = [
  'bubbles',
  'cancelBubble',
  'cancelable',
  'composed',
  'currentTarget',
  'defaultPrevented',
  'eventPhase',
  'isTrusted',
  'returnValue',
  'srcElement',
  'target',
  'timeStamp',
  'type',
];

export const legacyAttributesAreOwnDataProperties = {
  test() {
    for (const event of [new Event('x'), new CustomEvent('x')]) {
      deepStrictEqual(Object.keys(event).sort(), kAttributes);
      for (const name of kAttributes) {
        strictEqual(
          Object.getOwnPropertyDescriptor(Event.prototype, name),
          undefined,
          name
        );
        const { get, set, writable, enumerable, configurable } =
          Object.getOwnPropertyDescriptor(event, name);
        deepStrictEqual(
          { get, set, writable, enumerable, configurable },
          {
            get: undefined,
            set: undefined,
            writable: name === 'cancelBubble',
            enumerable: true,
            configurable: true,
          },
          name
        );
      }
    }
    const detail = Object.getOwnPropertyDescriptor(
      CustomEvent.prototype,
      'detail'
    );
    strictEqual(typeof detail.get, 'function');
  },
};

export const legacyAttributesFollowTheEventsState = {
  test() {
    const event = new Event('x', { cancelable: true });
    strictEqual(event.defaultPrevented, false);
    event.preventDefault();
    strictEqual(event.defaultPrevented, true);
    strictEqual(event.returnValue, false);
    event.stopPropagation();
    strictEqual(event.cancelBubble, true);
  },
};

// The setter converts the value; the property keeps no value of its own.
export const legacyCancelBubbleAssignmentRunsTheSetter = {
  test() {
    const event = new Event('x');
    event.cancelBubble = 'yes';
    strictEqual(event.cancelBubble, true);
  },
};

// The instance's own property shadows the subclass's accessor.
export const legacySubclassCannotOverrideAnAttribute = {
  test() {
    class MyEvent extends Event {
      get type() {
        return 'overridden';
      }
    }
    strictEqual(new MyEvent('x').type, 'x');
  },
};

export const legacyTargetsBeforeDispatch = {
  test() {
    const event = new Event('x');
    strictEqual(event.target, undefined);
    strictEqual(event.currentTarget, undefined);
    strictEqual(event.srcElement, null);
  },
};
