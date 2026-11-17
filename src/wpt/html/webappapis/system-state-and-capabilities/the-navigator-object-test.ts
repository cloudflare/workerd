// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'clientinformation.window.js': {
    comment: 'window.clientInformation only exists on Window',
    omittedTests: true,
  },
  'historical.https.window.js': {
    comment: 'registerProtocolHandler() only exists on the Window navigator',
    omittedTests: ['registerProtocolHandler has no third argument'],
  },
  'navigator.any.js': {
    comment:
      'navigator does not implement the NavigatorID attributes appCodeName, appName, ' +
      'appVersion and product. The userAgent value test needs XMLHttpRequest',
    expectedFailures: [
      'appCodeName',
      'appName',
      'appVersion',
      'product',
      'userAgent value',
    ],
  },
  'per-global.window.js': {
    comment: 'Compares navigator across windows and iframes',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
