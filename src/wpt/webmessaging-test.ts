// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

// Several tests call addEventListener('message', handler, true). workerd's EventTarget rejects
// useCapture=true, and capture is meaningless for a MessagePort, which has no DOM ancestors.
// Pattern: addEventListener("message", handler, true) -> addEventListener("message", handler)
function removeUseCapture(code: string): string {
  return code.replace(/,\s*true\s*\)/g, ')');
}

export default {
  'Channel_postMessage_Blob.any.js': {
    comment: 'FileReader is not implemented in workerd',
    expectedFailures: ['Test Description: Messages can contain Blobs.'],
    replace: removeUseCapture,
  },
  'Channel_postMessage_DataCloneErr.any.js': {},
  'Channel_postMessage_clone_port.any.js': {
    comment:
      'MessagePort.postMessage() rejects any transfer list, so the top-level postMessage() ' +
      'that transfers a port throws and the file never registers a result',
    disabledTests: true,
  },
  'Channel_postMessage_clone_port_error.any.js': {
    comment:
      'MessagePort.postMessage() rejects any transfer list with a plain Error before it ' +
      'checks for the source port, so no DataCloneError is thrown',
    expectedFailures: [
      'Test Description: Throw a DataCloneError if transfer array in postMessage contains source port.',
    ],
  },
  'Channel_postMessage_event_properties.any.js': {
    replace: removeUseCapture,
  },
  'Channel_postMessage_ports_readonly_array.any.js': {
    comment:
      'MessagePort.postMessage() rejects any transfer list, so the top-level postMessage() ' +
      'that transfers a port throws and the file never registers a result',
    disabledTests: true,
  },
  'Channel_postMessage_target_source.any.js': {
    replace: removeUseCapture,
  },
  'Channel_postMessage_transfer_xsite_incoming_messages.window.js': {
    comment: 'Requires document and cross-site iframes',
    omittedTests: true,
  },
  'Channel_postMessage_with_transfer_entangled.any.js': {
    comment: 'MessagePort.postMessage() does not support transferring ports',
    expectedFailures: [
      'An entangled port transferred to the same origin receives messages in order',
    ],
  },
  'Channel_postMessage_with_transfer_incoming_messages.any.js': {
    comment: 'MessagePort.postMessage() does not support transferring ports',
    expectedFailures: [
      'When transferring a non-enabled port mutiple times,\n    incoming messages sent at various transfer steps are received in order upon enablement.',
    ],
  },
  'Channel_postMessage_with_transfer_outgoing_messages.any.js': {
    comment: 'MessagePort.postMessage() does not support transferring ports',
    expectedFailures: [
      'When transferring a port,\n    outgoing messages sent at each transfer step are received in order by the entangled port.',
    ],
  },
  'MessageEvent-trusted.any.js': {
    comment:
      'BroadcastChannel and the Worker constructor are not implemented in workerd',
    disabledTests: ['With a BroadcastChannel'],
  },
  'MessageEvent-trusted.window.js': {
    comment: 'Requires window.postMessage',
    omittedTests: true,
  },
  'MessageEvent.any.js': {},
  'MessagePort_initial_disabled.any.js': {
    comment:
      'Like Node.js, workerd starts a port when any message listener is added, including ' +
      'via addEventListener(); per spec only start() or setting onmessage enables the port ' +
      'message queue. See MessagePort::listenerCountChanged in api/messagechannel.c++.',
    expectedFailures: [''],
    replace: removeUseCapture,
  },
  'MessagePort_onmessage_start.any.js': {},
  'broadcastchannel/basics.any.js': {
    comment: 'BroadcastChannel is not implemented in workerd',
    disabledTests: true,
  },
  'broadcastchannel/interface.any.js': {
    comment: 'BroadcastChannel is not implemented in workerd',
    disabledTests: true,
  },
  'broadcastchannel/origin.window.js': {
    comment:
      'Requires document and iframes; BroadcastChannel is not implemented',
    omittedTests: true,
  },
  'message-channels/basics.any.js': {},
  'message-channels/close.any.js': {
    comment:
      'MessagePort.postMessage() rejects any transfer list, so ports cannot be transferred ' +
      'and closed ports are not detected as detached (no DataCloneError)',
    expectedFailures: [
      'Message sent to closed port from transferred port should not arrive.',
      'close() detaches a MessagePort (but not the one its entangled with)',
    ],
  },
  'message-channels/detached-iframe.window.js': {
    comment: 'Requires iframes and service worker helpers',
    omittedTests: true,
  },
  'message-channels/dictionary-transferrable.any.js': {
    comment:
      'MessagePort.postMessage() rejects any transfer list, even a plain ArrayBuffer ' +
      '(api/messagechannel.c++, "Transfer list is not supported")',
    expectedFailures: [''],
  },
  'message-channels/historical.any.js': {
    comment:
      'Like Node.js, workerd dispatches a close event on both ports when one is closed ' +
      '(MessagePort::close in api/messagechannel.c++); the spec removed that event',
    expectedFailures: [
      'No close event should be fired when an entangled port is closed',
      'No close event should be fired on a port when it is closed',
    ],
  },
  'message-channels/implied-start.any.js': {},
  'message-channels/no-start.any.js': {
    comment:
      'Like Node.js, workerd starts a port when a message listener is added with ' +
      'addEventListener(); per spec the port stays disabled until start() is called. ' +
      'See MessagePort::listenerCountChanged in api/messagechannel.c++.',
    expectedFailures: [''],
  },
  'message-channels/worker-post-after-close.any.js': {
    comment:
      'Requires the dedicated Worker constructor, which workerd does not have',
    omittedTests: true,
  },
  'message-channels/worker.any.js': {
    comment:
      'Requires the dedicated Worker constructor, which workerd does not have',
    omittedTests: true,
  },
  'postMessage_MessagePorts_xsite.sub.window.js': {
    comment: 'Requires document and cross-site iframes',
    omittedTests: true,
  },
  'support/MessageEvent-trusted-worker.js': {
    comment:
      'Helper loaded by a Worker in MessageEvent-trusted.any.js, not a test',
    omittedTests: true,
  },
  'support/compare.js': {
    comment: 'Helper for the .html postMessage tests, not a test',
    omittedTests: true,
  },
  'without-ports/025-1.js': {
    comment: 'Worker script for without-ports/025.html, not a test',
    omittedTests: true,
  },
  'worker_postMessage_user_activation.js': {
    comment:
      'Worker script for worker_postMessage_user_activation.tentative.html, not a test',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
