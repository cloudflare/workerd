// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'AddEventListenerOptions-once.any.js': {},
  'AddEventListenerOptions-passive.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      'Supports passive option on addEventListener only',
      'preventDefault should be ignored if-and-only-if the passive option is true',
      'passive behavior of one listener should be unaffected by the presence of other listeners',
      'Equivalence of option values',
      'returnValue should be ignored if-and-only-if the passive option is true',
    ],
  },
  'AddEventListenerOptions-signal.any.js': {
    comment: 'addEventListener(): options.capture must be false.',
    expectedFailures: [
      'Passing an AbortSignal to addEventListener works with the capture flag',
    ],
  },
  'Event-constructors.any.js': {
    comment: 'TODO this is triggering a harness bug',
    skipAllTests: true,
  },
  'Event-dispatch-listener-order.window.js': {
    comment: 'document is not defined',
    // There is a single test, whose name is the empty string
    expectedFailures: [''],
  },
  'Event-isTrusted.any.js': {
    comment: 'Fix notStrictEqual for undefined, undefined',
    // There is a single test, whose name is the empty string
    expectedFailures: [''],
  },
  'EventListener-addEventListener.sub.window.js': {
    comment: 'document is not defined',
    expectedFailures: [
      "EventListener.addEventListener doesn't throw when a cross origin object is passed in.",
    ],
  },
  'EventTarget-add-remove-listener.any.js': {},
  'EventTarget-addEventListener.any.js': {
    comment:
      "Failed to execute 'addEventListener' on 'EventTarget': parameter 2 is not of type 'function or HandlerObject'.",
    expectedFailures: ['Adding a null event listener should succeed'],
  },
  'EventTarget-constructible.any.js': {
    comment: 'Should be null, not EventTarget',
    expectedFailures: [
      'A constructed EventTarget implements dispatch correctly',
    ],
  },
  'EventTarget-removeEventListener.any.js': {
    comment:
      "Failed to execute 'removeEventListener' on 'EventTarget': parameter 2 is not of type 'Object'.",
    expectedFailures: ['removing a null event listener should succeed'],
  },
  'event-global-extra.window.js': {
    comment: 'ReferenceError: document is not defined',
    skipAllTests: true,
  },
  'event-global-set-before-handleEvent-lookup.window.js': {
    comment: 'window is not defined',
    expectedFailures: ["window.event is set before 'handleEvent' lookup"],
  },
  'event-global.worker.js': {
    comment: 'ReferenceError: importScripts is not defined',
    skipAllTests: true,
  },
  'legacy-pre-activation-behavior.window.js': {
    comment: 'ReferenceError: document is not defined',
    expectedFailures: ['Use NONE phase during legacy-pre-activation behavior'],
  },
  'relatedTarget.window.js': {
    comment: 'ReferenceError: document is not defined',
    skipAllTests: true,
  },
  'scrolling/scroll_support.js': {
    comment: 'Only used by HTML files',
    skipAllTests: true,
  },
  'scrolling/scrollend-user-scroll-common.js': {
    comment: 'Only used by HTML files',
    skipAllTests: true,
  },
} satisfies TestRunnerConfig;
