// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

// The TypeScript EventSource (the TS_EVENT_SOURCE autogate) matches the C++
// one's shape, but its functions are not native.
const usingTsEventSource = !(
  EventSource as unknown as { from: () => void }
).from
  .toString()
  .includes('[native code]');
const kFailsWithCppEventSource = usingTsEventSource ? [] : true;

const kListenerThis =
  "Inside an EventSource listener, `this` is not the EventSource, so the test's this.close() throws";

export default {
  'dedicated-worker/eventsource-close.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-close2.js': {
    comment: 'Uses self.close() which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-constructor-no-new.any.js': {},
  'dedicated-worker/eventsource-constructor-non-same-origin.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-constructor-url-bogus.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-eventtarget.worker.js': {
    comment: 'Uses importScripts which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-onmesage.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-onopen.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-prototype.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'dedicated-worker/eventsource-url.js': {
    comment: 'Uses postMessage which is not available in workerd',
    omittedTests: true,
  },
  'event-data.any.js': {
    comment: 'The C++ EventSource drops an event whose data is empty',
    expectedFailures: kFailsWithCppEventSource,
  },
  'eventsource-close.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-document-domain.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-empty-url.any.js': {},
  'eventsource-constructor-non-same-origin.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-stringify.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-url-bogus.any.js': {},
  'eventsource-cross-origin-redirect-origin.any.js': {
    comment:
      'fetch() sends no Origin header, so the cross-origin cors.py handler fails with a 500',
    expectedFailures: [
      'MessageEvent.origin is the origin of the final URL after a cross-origin redirect',
    ],
  },
  'eventsource-cross-origin.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-eventtarget.any.js': {},
  'eventsource-onmessage-trusted.any.js': {},
  'eventsource-onmessage.any.js': {},
  'eventsource-onopen.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'eventsource-prototype.any.js': {},
  'eventsource-reconnect.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-request-cancellation.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-url.any.js': {},
  'format-bom-2.any.js': {},
  'format-bom-split.any.js': {
    comment:
      'The C++ EventSource does not strip a byte order mark split across chunks',
    expectedFailures: kFailsWithCppEventSource,
  },
  'format-bom-utf-16.any.js': {},
  'format-bom.any.js': {},
  'format-comments.any.js': {},
  'format-data-before-final-empty-line.any.js': {
    comment:
      'The C++ EventSource sets lastEventId from an event that is never dispatched',
    expectedFailures: kFailsWithCppEventSource,
  },
  'format-field-data.any.js': {
    comment: 'The C++ EventSource drops an event whose data is empty',
    expectedFailures: kFailsWithCppEventSource,
  },
  'format-field-event-empty.any.js': {
    comment: `${kListenerThis}, and with the C++ EventSource the file hangs`,
    disabledTests: true,
  },
  'format-field-event.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'format-field-id-2.any.js': {},
  'format-field-id-3.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'format-field-id-null.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'format-field-id-redirect.any.js': {},
  'format-field-id.any.js': {},
  'format-field-parsing.any.js': {},
  'format-field-retry-bogus-2.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'format-field-retry-bogus.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'format-field-retry-empty.any.js': {},
  'format-field-retry.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'format-field-unknown.any.js': {},
  'format-leading-space.any.js': {},
  'format-mime-bogus.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'format-mime-trailing-semicolon.any.js': {},
  'format-mime-valid-bogus.any.js': {},
  'format-newlines.any.js': {},
  'format-null-character.any.js': {},
  'format-utf-8.any.js': {},
  'request-accept.any.js': {
    comment: kListenerThis,
    expectedFailures: true,
  },
  'request-cache-control.any.js': {
    comment: `${kListenerThis}, so the second subtest never starts and the file hangs`,
    disabledTests: true,
  },
  'request-credentials.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'request-redirect.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'request-status-error.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'shared-worker/eventsource-close.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-constructor-non-same-origin.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-constructor-url-bogus.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-eventtarget.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-onmesage.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-onopen.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-prototype.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
  'shared-worker/eventsource-url.js': {
    comment: 'SharedWorker tests are not applicable to workerd',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
