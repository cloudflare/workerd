// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

// The dom/events WPT suite against the TypeScript Event, CustomEvent and
// EventTarget (src/per_isolate/events/). Expectations match the C++
// configuration (events-test.ts) except where the TypeScript classes follow
// the spec and the C++ ones do not.
export default {
  'AddEventListenerOptions-once.any.js': {},
  'AddEventListenerOptions-passive.any.js': {},
  'AddEventListenerOptions-signal.any.js': {},
  'Event-constructors.any.js': {},
  'Event-dispatch-listener-order.window.js': {
    comment: 'Test requires DOM',
    // There is a single test, whose name is the empty string
    omittedTests: [''],
  },
  'Event-isTrusted.any.js': {},
  'EventListener-addEventListener.sub.window.js': {
    comment: 'Only relevant to browsers',
    omittedTests: [
      "EventListener.addEventListener doesn't throw when a cross origin object is passed in.",
    ],
  },
  'EventTarget-add-remove-listener.any.js': {},
  'EventTarget-addEventListener.any.js': {},
  'EventTarget-constructible.any.js': {},
  'EventTarget-removeEventListener.any.js': {
    comment:
      'The test removes a capture listener from the global, which is still a C++ EventTarget',
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
