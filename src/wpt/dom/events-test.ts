// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'AddEventListenerOptions-once.any.js': {},
  'AddEventListenerOptions-passive.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      'preventDefault should be ignored if-and-only-if the passive option is true',
      'passive behavior of one listener should be unaffected by the presence of other listeners',
      'Equivalence of option values',
      'returnValue should be ignored if-and-only-if the passive option is true',
    ],
  },
  'AddEventListenerOptions-signal.any.js': {
    comment: 'capture is not relevant outside of the DOM',
    omittedTests: [
      'Passing an AbortSignal to addEventListener works with the capture flag',
    ],
  },
  'Event-constructors.any.js': {
    comment: 'TODO this is triggering a harness bug',
    disabledTests: true,
  },
  'Event-dispatch-listener-order.window.js': {
    comment: 'Test requires DOM',
    // There is a single test, whose name is the empty string
    omittedTests: [''],
  },
  'Event-isTrusted.any.js': {
    comment: 'isTrusted must be an own property, not a prototype property',
    // With the pedantic_wpt compat flag we actually set the isTrusted property
    // to be an own property. However, it's an own property that uses a value
    // in the descriptor rather than a getter and the test is specifically looking
    // for a getter for some reason. That's way more pedantic than we want to
    // deal with so just omit the test for now.
    omittedTests: [''],
  },
  'EventListener-addEventListener.sub.window.js': {
    comment: 'Only relevant to browsers',
    omittedTests: [
      "EventListener.addEventListener doesn't throw when a cross origin object is passed in.",
    ],
  },
  'EventTarget-add-remove-listener.any.js': {},
  'EventTarget-addEventListener.any.js': {},
  'EventTarget-constructible.any.js': {
    comment: 'Should be null, not EventTarget',
    expectedFailures: [],
  },
  'EventTarget-removeEventListener.any.js': {
    comment: 'capture is not relevant outside of the DOM',
    expectedFailures: ['removing a null event listener should succeed'],
  },
  'event-global-extra.window.js': {
    comment: 'Test is DOM-specific',
    disabledTests: true,
  },
  'event-global-set-before-handleEvent-lookup.window.js': {
    comment: 'window is not defined',
    expectedFailures: ["window.event is set before 'handleEvent' lookup"],
  },
  'event-global.worker.js': {
    comment: 'ReferenceError: importScripts is not defined',
    disabledTests: true,
  },
  'legacy-pre-activation-behavior.window.js': {
    comment: 'Only relevant to browsers',
    omittedTests: ['Use NONE phase during legacy-pre-activation behavior'],
  },
  'relatedTarget.window.js': {
    comment: 'Test is DOM-specific',
    omittedTests: true,
  },
  'scrolling/scroll_support.js': {
    comment: 'Only used by HTML files',
    omittedTests: true,
  },
  'scrolling/scrollend-user-scroll-common.js': {
    comment: 'Only used by HTML files',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
