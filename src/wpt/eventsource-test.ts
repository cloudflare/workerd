// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

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
  'event-data.any.js': {},
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
  'eventsource-onopen.any.js': {},
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
  'format-bom-split.any.js': {},
  'format-bom-utf-16.any.js': {},
  'format-bom.any.js': {},
  'format-comments.any.js': {},
  'format-data-before-final-empty-line.any.js': {},
  'format-field-data.any.js': {},
  'format-field-event-empty.any.js': {},
  'format-field-event.any.js': {},
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
  'format-field-retry-bogus-2.any.js': {},
  'format-field-retry-bogus.any.js': {},
  'format-field-retry-empty.any.js': {},
  'format-field-retry.any.js': {},
  'format-field-unknown.any.js': {},
  'format-leading-space.any.js': {},
  'format-mime-bogus.any.js': {},
  'format-mime-trailing-semicolon.any.js': {},
  'format-mime-valid-bogus.any.js': {},
  'format-newlines.any.js': {},
  'format-null-character.any.js': {},
  'format-utf-8.any.js': {},
  'request-accept.any.js': {},
  'request-cache-control.any.js': {
    comment:
      'fetch() sends no Origin header, so the cross-origin cors.py handler fails with a 500 and the file hangs',
    disabledTests: [/cors\.py\?run=cache-control/],
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
