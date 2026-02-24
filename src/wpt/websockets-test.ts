// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

// The WPT WebSocket tests use addEventListener with useCapture=true, which workerd
// doesn't support. This function removes the useCapture argument from those calls.
// Pattern: addEventListener('event', handler, true) -> addEventListener('event', handler)
function removeUseCapture(code: string): string {
  return code.replace(/,\s*true\s*\)/g, ')');
}

export default {
  'Close-1000-reason.any.js': {
    replace: removeUseCapture,
  },
  'Close-1000-verify-code.any.js': {
    replace: removeUseCapture,
  },
  'Close-1000.any.js': {
    replace: removeUseCapture,
  },
  'Close-1005-verify-code.any.js': {
    replace: removeUseCapture,
  },
  'Close-1005.any.js': {
    replace: (code: string): string => {
      code = removeUseCapture(code);
      // The test expects close(1005) to throw, leaving the WebSocket open.
      // In workerd, the open connection's read loop is registered as a
      // waitUntil task that keeps the IoContext alive indefinitely.
      // Add a cleanup to close the WebSocket when the test completes so the
      // connection is properly shut down.
      return code.replace(
        'var isOpenCalled = false;',
        'var isOpenCalled = false;\ntest.add_cleanup(function() { wsocket.close(); });'
      );
    },
  },
  'Close-2999-reason.any.js': {
    comment: 'workerd does not throw INVALID_ACCESS_ERR for close(2999)',
    expectedFailures: [
      'Create WebSocket - Close the Connection - close(2999, reason) - INVALID_ACCESS_ERR is thrown',
    ],
    replace: removeUseCapture,
  },
  'Close-3000-reason.any.js': {
    replace: removeUseCapture,
  },
  'Close-3000-verify-code.any.js': {
    replace: removeUseCapture,
  },
  'Close-4999-reason.any.js': {
    replace: removeUseCapture,
  },
  'Close-Reason-124Bytes.any.js': {
    comment: 'workerd does not throw SYNTAX_ERR for reason > 123 bytes',
    expectedFailures: [
      "Create WebSocket - Close the Connection - close(code, 'reason more than 123 bytes') - SYNTAX_ERR is thrown",
    ],
    replace: removeUseCapture,
  },
  'Close-delayed.any.js': {
    replace: removeUseCapture,
  },
  'Close-onlyReason.any.js': {
    comment:
      'workerd throws TypeError instead of INVALID_ACCESS_ERR for close(undefined, reason), test hangs',
    disabledTests: true,
  },
  'Close-readyState-Closed.any.js': {
    replace: removeUseCapture,
  },
  'Close-readyState-Closing.any.js': {
    replace: removeUseCapture,
  },
  'Close-reason-unpaired-surrogates.any.js': {
    comment: 'workerd handles unpaired surrogates differently',
    expectedFailures: [
      'Create WebSocket - Close the Connection - close(reason with unpaired surrogates) - connection should get closed',
    ],
    replace: removeUseCapture,
  },
  'Close-server-initiated-close.any.js': {
    comment:
      'readyState is CLOSING (2) instead of CLOSED (3) when close event fires',
    expectedFailures: [
      'Create WebSocket - Server initiated Close - Client sends back a CLOSE - readyState should be in CLOSED state and wasClean is TRUE - Connection should be closed',
    ],
    replace: removeUseCapture,
  },
  'Close-undefined.any.js': {
    replace: removeUseCapture,
  },
  'Create-asciiSep-protocol-string.any.js': {},
  'Create-blocked-port.any.js': {
    comment: 'Port blocking works differently in workerd',
    disabledTests: true,
  },
  'Create-extensions-empty.any.js': {
    comment: 'workerd WebSocket.extensions is null instead of empty string',
    expectedFailures: [
      "Create WebSocket - wsocket.extensions should be set to '' after connection is established - Connection should be closed",
    ],
    replace: removeUseCapture,
  },
  'Create-http-urls.any.js': {
    comment: 'workerd requires ws/wss scheme, not http/https',
    expectedFailures: ['WebSocket: ensure both HTTP schemes are supported'],
  },
  'Create-invalid-urls.any.js': {},
  'Create-non-absolute-url.any.js': {
    comment: 'workerd throws SyntaxError for non-absolute URLs',
    expectedFailures: [
      'Create WebSocket - Pass a non absolute URL: test',
      'Create WebSocket - Pass a non absolute URL: ?',
      'Create WebSocket - Pass a non absolute URL: null',
      'Create WebSocket - Pass a non absolute URL: 123',
    ],
  },
  'Create-nonAscii-protocol-string.any.js': {},
  'Create-on-worker-shutdown.any.js': {
    comment: 'Worker shutdown behavior is different in workerd',
    disabledTests: true,
  },
  'Create-protocol-with-space.any.js': {},
  'Create-protocols-repeated-case-insensitive.any.js': {
    comment:
      'workerd does not throw for case-insensitive duplicate protocols (allows "Echo" and "echo")',
    expectedFailures: [
      'Create WebSocket - Pass a valid URL and an array of protocol strings with repeated values but different case - SYNTAX_ERR is thrown',
    ],
  },
  'Create-protocols-repeated.any.js': {},
  'Create-url-with-space.any.js': {},
  'Create-valid-url-array-protocols.any.js': {
    replace: removeUseCapture,
  },
  'Create-valid-url-binaryType-blob.any.js': {
    comment:
      'workerd WebSocket binaryType defaults to "arraybuffer", not "blob"; test hangs',
    disabledTests: true,
  },
  'Create-valid-url-protocol-empty.any.js': {
    replace: removeUseCapture,
  },
  'Create-valid-url-protocol-setCorrectly.any.js': {
    replace: removeUseCapture,
  },
  'Create-valid-url-protocol-string.any.js': {
    replace: removeUseCapture,
  },
  'Create-valid-url-protocol.any.js': {
    replace: removeUseCapture,
  },
  'Create-valid-url.any.js': {
    replace: removeUseCapture,
  },
  'Send-0byte-data.any.js': {
    comment:
      'workerd returns 0 instead of undefined for empty message data; test hangs',
    disabledTests: true,
  },
  'Send-65K-data.any.js': {
    comment:
      'workerd returns byte count instead of undefined for bufferedAmount; test hangs',
    disabledTests: true,
  },
  'Send-before-open.any.js': {
    comment:
      'workerd throws TypeError instead of InvalidStateError DOMException for send() before open',
    expectedFailures: [
      'Send data on a WebSocket before connection is opened - INVALID_STATE_ERR is returned',
    ],
  },
  // The following Send tests all check bufferedAmount which returns byte count in workerd
  // instead of undefined as expected by the spec. They also hang after failures.
  'Send-binary-65K-arraybuffer.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybuffer.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-float16.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-float32.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-float64.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-int16-offset.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-int32.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-int8.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-uint16-offset-length.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-uint32-offset.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-uint8-offset-length.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-arraybufferview-uint8-offset.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-binary-blob.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-data.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-data.worker.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-null.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-paired-surrogates.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-unicode-data.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'Send-unpaired-surrogates.any.js': {
    comment:
      'bufferedAmount returns byte count instead of undefined; test hangs',
    disabledTests: true,
  },
  'back-forward-cache-with-closed-websocket-connection.window.js': {
    comment: 'Back/forward cache tests are browser-specific',
    omittedTests: true,
  },
  'back-forward-cache-with-open-websocket-connection-and-close-it-in-pagehide.window.js':
    {
      comment: 'Back/forward cache tests are browser-specific',
      omittedTests: true,
    },
  'back-forward-cache-with-open-websocket-connection.window.js': {
    comment: 'Back/forward cache tests are browser-specific',
    omittedTests: true,
  },
  'basic-auth.any.js': {
    comment: 'Basic authentication for WebSockets is not supported',
    disabledTests: true,
  },
  'binaryType-wrong-value.any.js': {
    comment: 'binaryType is undefined instead of blob in workerd; test hangs',
    disabledTests: true,
  },
  'bufferedAmount-unchanged-by-sync-xhr.any.js': {
    comment: 'Synchronous XHR is not supported in Workers',
    omittedTests: true,
  },
  'close-invalid.any.js': {
    replace: removeUseCapture,
  },
  'constants.sub.js': {},
  'constructor.any.js': {},
  'cookies/support/websocket-cookies-helper.sub.js': {
    comment: 'Cookie support helper, not an actual test',
    omittedTests: true,
  },
  'eventhandlers.any.js': {
    comment: 'TreatNonCallableAsNull behavior differs from spec',
    expectedFailures: [
      'Event handler for open should have [TreatNonCallableAsNull]',
      'Event handler for error should have [TreatNonCallableAsNull]',
      'Event handler for close should have [TreatNonCallableAsNull]',
      'Event handler for message should have [TreatNonCallableAsNull]',
    ],
  },
  'idlharness.any.js': {
    comment: 'TODO: Investigate this',
    expectedFailures: [
      'WebSocket interface: existence and properties of interface object',
      'WebSocket interface object length',
      'WebSocket interface object name',
      'WebSocket interface: existence and properties of interface prototype object',
      'WebSocket interface: existence and properties of interface prototype object\'s "constructor" property',
      "WebSocket interface: existence and properties of interface prototype object's @@unscopables property",
      'WebSocket interface: attribute url',
      'WebSocket interface: constant CONNECTING on interface object',
      'WebSocket interface: constant CONNECTING on interface prototype object',
      'WebSocket interface: constant OPEN on interface object',
      'WebSocket interface: constant OPEN on interface prototype object',
      'WebSocket interface: constant CLOSING on interface object',
      'WebSocket interface: constant CLOSING on interface prototype object',
      'WebSocket interface: constant CLOSED on interface object',
      'WebSocket interface: constant CLOSED on interface prototype object',
      'WebSocket interface: attribute readyState',
      'WebSocket interface: attribute bufferedAmount',
      'WebSocket interface: attribute onopen',
      'WebSocket interface: attribute onerror',
      'WebSocket interface: attribute onclose',
      'WebSocket interface: attribute extensions',
      'WebSocket interface: attribute protocol',
      'WebSocket interface: operation close(optional unsigned short, optional USVString)',
      'WebSocket interface: attribute onmessage',
      'WebSocket interface: attribute binaryType',
      'WebSocket interface: operation send((BufferSource or Blob or USVString))',
      'WebSocket must be primary interface of new WebSocket("ws://invalid")',
      'Stringification of new WebSocket("ws://invalid")',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "url" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "CONNECTING" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "OPEN" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "CLOSING" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "CLOSED" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "readyState" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "bufferedAmount" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "onopen" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "onerror" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "onclose" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "extensions" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "protocol" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "close(optional unsigned short, optional USVString)" with the proper type',
      'WebSocket interface: calling close(optional unsigned short, optional USVString) on new WebSocket("ws://invalid") with too few arguments must throw TypeError',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "onmessage" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "binaryType" with the proper type',
      'WebSocket interface: new WebSocket("ws://invalid") must inherit property "send((BufferSource or Blob or USVString))" with the proper type',
      'WebSocket interface: calling send((BufferSource or Blob or USVString)) on new WebSocket("ws://invalid") with too few arguments must throw TypeError',
      'CloseEvent interface: existence and properties of interface object',
      'CloseEvent interface object length',
      'CloseEvent interface object name',
      'CloseEvent interface: existence and properties of interface prototype object',
      'CloseEvent interface: existence and properties of interface prototype object\'s "constructor" property',
      "CloseEvent interface: existence and properties of interface prototype object's @@unscopables property",
      'CloseEvent interface: attribute wasClean',
      'CloseEvent interface: attribute code',
      'CloseEvent interface: attribute reason',
      'CloseEvent must be primary interface of new CloseEvent("close")',
      'Stringification of new CloseEvent("close")',
      'CloseEvent interface: new CloseEvent("close") must inherit property "wasClean" with the proper type',
      'CloseEvent interface: new CloseEvent("close") must inherit property "code" with the proper type',
      'CloseEvent interface: new CloseEvent("close") must inherit property "reason" with the proper type',
    ],
  },
  'interfaces/WebSocket/close/close-connecting-async.any.js': {
    comment:
      'readyState is CONNECTING (1) instead of CLOSING (2) after close()',
    expectedFailures: [
      'close event should be fired asynchronously when WebSocket is connecting',
    ],
  },
  'mixed-content.https.any.js': {
    comment: 'Mixed content checks are browser-specific',
    omittedTests: true,
  },
  'opening-handshake/003-sets-origin.worker.js': {
    comment: 'importScripts is not available in Workers',
    omittedTests: true,
  },
  'referrer.any.js': {
    comment: 'Referrer behavior is different in workers',
    disabledTests: true,
  },
  'remove-own-iframe-during-onerror.window.js': {
    comment: 'iframe tests are browser-specific',
    omittedTests: true,
  },
  'send-many-64K-messages-with-backpressure.any.js': {
    replace: removeUseCapture,
  },
} satisfies TestRunnerConfig;
