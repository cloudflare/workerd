// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type { TestRunnerConfig } from 'harness/harness'

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
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'eventsource-close.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-document-domain.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-empty-url.any.js': {
    comment:
      'workerd throws SyntaxError for empty URL instead of resolving to self.location',
    expectedFailures: ['EventSource constructor with an empty url.'],
  },
  'eventsource-constructor-non-same-origin.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-stringify.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-constructor-url-bogus.any.js': {},
  'eventsource-cross-origin.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-eventtarget.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'eventsource-onmessage-trusted.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'eventsource-onmessage.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'eventsource-onopen.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'eventsource-prototype.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'eventsource-reconnect.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-request-cancellation.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'eventsource-url.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-bom-2.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-bom.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-comments.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-data-before-final-empty-line.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-data.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-event-empty.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-event.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-id-2.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-id-3.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'format-field-id-null.window.js': {
    comment: 'Window-specific tests are not supported',
    omittedTests: true,
  },
  'format-field-id.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-parsing.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-retry-bogus.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-retry-empty.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-retry.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-field-unknown.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-leading-space.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-mime-bogus.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-mime-trailing-semicolon.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-mime-valid-bogus.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-newlines.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-null-character.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'format-utf-8.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'request-accept.any.js': {
    comment: 'EventSource does not support relative URLs in workerd',
    expectedFailures: true,
  },
  'request-cache-control.any.js': {
    comment: 'Test times out due to relative URL handling',
    omittedTests: true,
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
} satisfies TestRunnerConfig
