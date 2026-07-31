// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'idlharness.any.js': {},
  'piping/abort.any.js': {
    comment:
      'Microtask ordering: the async pump cannot detect source-close ' +
      'before a same-tick abort fires, so the abort wins the race ' +
      'against the spec condition-3 (source-close) shutdown.',
    expectedFailures: ['abort should do nothing after the readable is closed'],
  },
  'piping/close-propagation-backward.any.js': {},
  'piping/close-propagation-forward.any.js': {},
  'piping/error-propagation-backward.any.js': {},
  'piping/error-propagation-forward.any.js': {},
  'piping/flow-control.any.js': {
    comment:
      'Backpressure tracking: desiredSize not decremented during pipe writes due to differences in the way draining read works',
    expectedFailures: [
      'Piping to a WritableStream that does not consume the writes fast enough exerts backpressure on the ReadableStream',
    ],
  },
  'piping/general-addition.any.js': {},
  'piping/general.any.js': {},
  'piping/multiple-propagation.any.js': {},
  'piping/pipe-through.any.js': {},
  'piping/then-interception.any.js': {},
  'piping/throwing-options.any.js': {},
  'piping/transform-streams.any.js': {},

  'queuing-strategies-size-function-per-global.window.js': {
    comment: 'document is not defined',
    disabledTests: true,
  },
  'queuing-strategies.any.js': {},
  'readable-byte-streams/bad-buffers-and-views.any.js': {},
  'readable-byte-streams/construct-byob-request.any.js': {},
  'readable-byte-streams/crashtests/tee-locked-stream.any.js': {},
  'readable-byte-streams/enqueue-with-detached-buffer.any.js': {},
  'readable-byte-streams/general.any.js': {},
  'readable-byte-streams/non-transferable-buffers.any.js': {},
  'readable-byte-streams/patched-global.any.js': {
    runInGlobalScope: true,
  },
  'readable-byte-streams/read-min.any.js': {
    comment:
      'tee+min byobRequest null due to shared-queue multi-cursor model ' +
      '(byobRequest requires singleCursor). Intentional divergence.',
    expectedFailures: [
      'ReadableStream with byte source: tee() with read({ min }) from branch1 and read() from branch2',
    ],
  },
  'readable-byte-streams/respond-after-enqueue.any.js': {},
  'readable-byte-streams/tee.any.js': {
    comment:
      'AggregateError cancel reason (intentional divergence, tests hang). ' +
      'byobRequest null during tee (shared-queue multi-cursor model).',
    disabledTests: [
      // HANG: AggregateError cancel reason instead of spec [r1, r2] array.
      // Tests create an unresolved Promise on assertion failure and the
      // validate() Promise.all hangs the entire file.
      'ReadableStream teeing with byte source: canceling both branches should aggregate the cancel reasons into an array',
      'ReadableStream teeing with byte source: canceling both branches in reverse order should aggregate the cancel reasons into an array',
    ],
    expectedFailures: [
      // AggregateError cancel reason: cancel reason is AggregateError
      // instead of spec [r1, r2] array. These fail fast on assertion.
      'ReadableStream teeing with byte source: failing to cancel when canceling both branches in sequence with delay',
      'ReadableStream teeing with byte source: failing to cancel the original stream should cause cancel() to reject on branches',
      // Shared-queue tee model: pull count / pull sequencing differs
      // from the spec's per-branch clone model.
      'ReadableStream teeing with byte source: stops pulling when original stream errors while both branches are reading',
      // Shared-queue tee model: byte offset handling on cloned views
      // differs from spec's per-branch transfer.
      'ReadableStream teeing with byte source: reading an array with a byte offset should clone correctly',
      // byobRequest null during tee: shared-queue multi-cursor model
      // makes byobRequest unavailable. These fail fast (TypeError or
      // assertion), no hang risk.
      'ReadableStream teeing with byte source: chunks for BYOB requests from branch 1 should be cloned to branch 2',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch1, cancel branch2',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch2, cancel branch1',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch2, enqueue to branch1',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch1, respond to branch2',
      'ReadableStream teeing with byte source: pull with BYOB reader, then pull with default reader',
      'ReadableStream teeing with byte source: pull with default reader, then pull with BYOB reader',
      'ReadableStream teeing with byte source: read from branch2, then read from branch1',
      'ReadableStream teeing with byte source: close when both branches have pending BYOB reads',
      'ReadableStream teeing with byte source: respond() and close() while both branches are pulling',
    ],
  },
  'readable-byte-streams/templated.any.js': {},

  'readable-streams/async-iterator.any.js': {},
  'readable-streams/bad-strategies.any.js': {},
  'readable-streams/bad-underlying-sources.any.js': {},
  'readable-streams/cancel.any.js': {},
  'readable-streams/constructor.any.js': {},
  'readable-streams/count-queuing-strategy-integration.any.js': {},
  'readable-streams/crashtests/empty.js': {},
  'readable-streams/crashtests/garbage-collection.any.js': {},
  'readable-streams/crashtests/strategy-worker.js': {
    comment: 'ReferenceError: importScripts is not defined',
    disabledTests: true,
  },
  'readable-streams/cross-realm-crash.window.js': {
    comment: 'document is not defined',
    expectedFailures: [
      'should not crash on reading from stream cancelled in destroyed realm',
    ],
  },
  'readable-streams/default-reader.any.js': {},
  'readable-streams/floating-point-total-queue-size.any.js': {},
  'readable-streams/from.any.js': {
    comment:
      'Intentional divergence: we treat strings as single chunks, not code-point iterables',
    expectedFailures: [
      // INTENTIONAL SPEC DIVERGENCE: The spec iterates strings code-point-
      // by-code-point, but that is surprising to users and has terrible
      // performance. We treat strings as single chunks instead.
      'ReadableStream.from accepts a string',
    ],
  },
  'readable-streams/garbage-collection.any.js': {},
  'readable-streams/general.any.js': {},
  'readable-streams/owning-type-message-port.any.js': {
    comment:
      'Enable once MessageChannel/MessagePort is implemented and owning type is implemented',
    disabledTests: true,
  },
  'readable-streams/owning-type-video-frame.any.js': {
    comment: 'VideoFrame is not implemented',
    disabledTests: true,
  },
  'readable-streams/owning-type.any.js': {
    comment: "Type 'owning' is not part of the spec yet — not implemented",
    disabledTests: true,
  },
  'readable-streams/patched-global.any.js': {
    runInGlobalScope: true,
  },
  'readable-streams/read-task-handling.window.js': {
    comment: 'document is not defined',
    disabledTests: true,
  },
  'readable-streams/reentrant-strategies.any.js': {},
  'readable-streams/tee.any.js': {
    comment:
      'Intentional divergence: tee cancel uses AggregateError instead of spec array, ' +
      'and shared-queue architecture causes different pull counts vs spec clone model',
    disabledTests: true,
    // These two HANG: assert_array_equals(AggregateError, [...]) throws inside
    // the cancel callback, preventing the test promise from ever resolving.
    // 'ReadableStream teeing: canceling both branches should aggregate the cancel reasons into an array',
    // 'ReadableStream teeing: canceling both branches in reverse order should aggregate the cancel reasons into an array',
  },
  'readable-streams/templated.any.js': {},

  'transferable/deserialize-error.window.js': {
    comment: 'ReferenceError: document is not defined',
    disabledTests: true,
  },
  'transferable/transfer-with-messageport.window.js': {
    comment: 'Enable once MessagePort is supported.',
    expectedFailures: [
      'ReadableStream must not be serializable',
      'WritableStream must not be serializable',
      'TransformStream must not be serializable',
      'Transferring a MessagePort with a ReadableStream should set `.ports`',
      'Transferring a MessagePort with a WritableStream should set `.ports`',
      'Transferring a MessagePort with a TransformStream should set `.ports`',
      'Transferring a MessagePort with a ReadableStream should set `.ports`, advanced',
      'Transferring a MessagePort with a WritableStream should set `.ports`, advanced',
      'Transferring a MessagePort with a TransformStream should set `.ports`, advanced',
      'Transferring a MessagePort with multiple streams should set `.ports`',
    ],
  },
  'transferable/transform-stream-members.any.js': {
    comment: 'Appears to be about the wrong type of error',
    expectedFailures: [
      'Transferring [object TransformStream],[object ReadableStream] should fail',
      'Transferring [object ReadableStream],[object TransformStream] should fail',
      'Transferring [object TransformStream],[object WritableStream] should fail',
      'Transferring [object WritableStream],[object TransformStream] should fail',
    ],
  },

  'transform-streams/backpressure.any.js': {},
  'transform-streams/cancel.any.js': {},
  'transform-streams/errors.any.js': {},
  'transform-streams/flush.any.js': {},
  'transform-streams/general.any.js': {},
  'transform-streams/lipfuzz.any.js': {},
  'transform-streams/patched-global.any.js': {
    runInGlobalScope: true,
  },
  'transform-streams/properties.any.js': {},
  'transform-streams/reentrant-strategies.any.js': {},
  'transform-streams/strategies.any.js': {},
  'transform-streams/terminate.any.js': {},
  'writable-streams/aborting.any.js': {},
  'writable-streams/bad-strategies.any.js': {},
  'writable-streams/bad-underlying-sinks.any.js': {},
  'writable-streams/byte-length-queuing-strategy.any.js': {},
  'writable-streams/close.any.js': {},
  'writable-streams/constructor.any.js': {},
  'writable-streams/count-queuing-strategy.any.js': {},
  'writable-streams/crashtests/garbage-collection.any.js': {},
  'writable-streams/error.any.js': {},
  'writable-streams/floating-point-total-queue-size.any.js': {},
  'writable-streams/garbage-collection.any.js': {},
  'writable-streams/general.any.js': {},
  'writable-streams/properties.any.js': {},
  'writable-streams/reentrant-strategy.any.js': {},
  'writable-streams/start.any.js': {},
  'writable-streams/write.any.js': {},
} satisfies TestRunnerConfig;
