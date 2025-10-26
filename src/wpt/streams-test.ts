// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'idlharness.any.js': {
    comment: 'Test file /resources/WebIDLParser.js not found.',
    disabledTests: true,
  },

  'piping/abort.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      "(reason: 'null') all pending writes should complete on abort",
      "(reason: 'undefined') all pending writes should complete on abort",
      "(reason: 'error1: error1') all pending writes should complete on abort",
      'abort signal takes priority over errored writable',
      'a rejection from underlyingSource.cancel() should be returned by pipeTo()',
      'a rejection from underlyingSink.abort() should be preferred to one from underlyingSource.cancel()',
      'abort should do nothing after the readable is errored, even with pending writes',
      'abort should do nothing after the writable is errored',
      'pipeTo on a teed readable byte stream should only be aborted when both branches are aborted',
      "(reason: 'null') underlyingSource.cancel() should called when abort, even with pending pull",
      "(reason: 'undefined') underlyingSource.cancel() should called when abort, even with pending pull",
      "(reason: 'error1: error1') underlyingSource.cancel() should called when abort, even with pending pull",
    ],
  },
  'piping/close-propagation-backward.any.js': {
    comment: 'A hanging Promise was canceled.',
    disabledTests: true,
  },
  'piping/close-propagation-forward.any.js': {},
  'piping/error-propagation-backward.any.js': {
    comment: 'A hanging Promise was canceled.',
    disabledTests: true,
  },
  'piping/error-propagation-forward.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Errors must be propagated forward: starts errored; preventAbort = true (truthy)',
      'Errors must be propagated forward: starts errored; preventAbort = a (truthy)',
      'Errors must be propagated forward: starts errored; preventAbort = 1 (truthy)',
      'Errors must be propagated forward: starts errored; preventAbort = Symbol() (truthy)',
      'Errors must be propagated forward: starts errored; preventAbort = [object Object] (truthy)',
      'Errors must be propagated forward: starts errored; preventAbort = true, preventCancel = true',
      'Errors must be propagated forward: starts errored; preventAbort = true, preventCancel = true, preventClose = true',
      'Errors must be propagated forward: starts errored; preventAbort = false; rejected abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = false; fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = undefined (falsy); fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = null (falsy); fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = false (falsy); fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = 0 (falsy); fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = -0 (falsy); fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort = NaN (falsy); fulfilled abort promise',
      'Errors must be propagated forward: starts errored; preventAbort =  (falsy); fulfilled abort promise',
      'Errors must be propagated forward: shutdown must not occur until the final write completes; becomes errored after first write',
      'Errors must be propagated forward: shutdown must not occur until the final write completes; becomes errored after first write; preventAbort = true',
      'Errors must be propagated forward: becomes errored after one chunk; dest never desires chunks; preventAbort = false; fulfilled abort promise',
      'Errors must be propagated forward: becomes errored after one chunk; dest never desires chunks; preventAbort = false; rejected abort promise',
      'Errors must be propagated forward: becomes errored after one chunk; dest never desires chunks; preventAbort = true',
    ],
  },
  'piping/flow-control.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Piping from a non-empty ReadableStream into a WritableStream that does not desire chunks',
      'Piping from a non-empty ReadableStream into a WritableStream that does not desire chunks, but then does',
      'Piping from a ReadableStream to a WritableStream that desires more chunks before finishing with previous ones',
    ],
  },
  'piping/general-addition.any.js': {},
  'piping/general.any.js': {
    comment:
      'Illegal invocation: function called with incorrect `this` reference.',
    expectedFailures: [
      'pipeTo must check the brand of its ReadableStream this value',
    ],
  },
  'piping/multiple-propagation.any.js': {
    comment: 'TypeError: Cannot close a writer that is already being closed',
    expectedFailures: [
      'Piping from a closed readable stream to a closed writable stream',
    ],
  },
  'piping/pipe-through.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      "pipeThrough should brand-check readable and not allow 'null'",
      "pipeThrough should brand-check readable and not allow 'undefined'",
      "pipeThrough should brand-check readable and not allow '0'",
      "pipeThrough should brand-check readable and not allow 'NaN'",
      "pipeThrough should brand-check readable and not allow 'true'",
      "pipeThrough should brand-check readable and not allow 'ReadableStream'",
      "pipeThrough should brand-check readable and not allow '[object ReadableStream]'",
      "pipeThrough should brand-check writable and not allow 'null'",
      "pipeThrough should brand-check writable and not allow 'undefined'",
      "pipeThrough should brand-check writable and not allow '0'",
      "pipeThrough should brand-check writable and not allow 'NaN'",
      "pipeThrough should brand-check writable and not allow 'true'",
      "pipeThrough should brand-check writable and not allow 'WritableStream'",
      "pipeThrough should brand-check writable and not allow '[object WritableStream]'",
      'pipeThrough should rethrow errors from accessing readable or writable',
      'pipeThrough() should throw if readable/writable getters throw',
    ],
  },
  'piping/then-interception.any.js': {
    comment:
      'failed: expected Wrappable::tryUnwrapOpaque(isolate, handle) != nullptr',
    expectedFailures: [
      'piping should not be observable',
      'tee should not be observable',
    ],
  },
  'piping/throwing-options.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'pipeThrough should stop after getting preventAbort throws',
      'pipeThrough should stop after getting preventCancel throws',
      'pipeThrough should stop after getting preventClose throws',
      'pipeThrough should stop after getting signal throws',
      'pipeTo should stop after getting preventAbort throws',
      'pipeTo should stop after getting preventCancel throws',
      'pipeTo should stop after getting preventClose throws',
      'pipeTo should stop after getting signal throws',
    ],
  },
  'piping/transform-streams.any.js': {},

  'queuing-strategies-size-function-per-global.window.js': {
    comment: 'document is not defined',
    disabledTests: true,
  },
  'queuing-strategies.any.js': {
    comment: 'Likely missing validation',
    expectedFailures: [
      'CountQueuingStrategy: Constructor behaves as expected with strange arguments',
      'CountQueuingStrategy: size is the same function across all instances',
      'CountQueuingStrategy: size should have the right name',
      'CountQueuingStrategy: size should not have a prototype property',
      'ByteLengthQueuingStrategy: Constructor behaves as expected with strange arguments',
      'ByteLengthQueuingStrategy: size is the same function across all instances',
      'ByteLengthQueuingStrategy: size should have the right name',
      'ByteLengthQueuingStrategy: size should not have a prototype property',
      'CountQueuingStrategy: size should not be a constructor',
      'ByteLengthQueuingStrategy: size should not be a constructor',
      'ByteLengthQueuingStrategy: size should have the right length',
      'ByteLengthQueuingStrategy: size behaves as expected with strange arguments',
    ],
  },

  'readable-byte-streams/bad-buffers-and-views.any.js': {
    comment: 'See individual comments',
    expectedFailures: [
      "ReadableStream with byte source: respond() throws if the BYOB request's buffer has been detached (in the closed state)",
      // TODO(conform): The spec expects us to throw here because the supplied view
      // has a different offset. Instead, we allow it because the view is zero length
      // and the controller has been closed (we do the close and zero length check)
      // first.
      // assert_throws_js(RangeError, () => c.byobRequest.respondWithNewView(view));
      'ReadableStream with byte source: respondWithNewView() throws if the supplied view has a different offset (in the closed state)',
      // TODO(conform): The spec expects this to be a RangeError
      "ReadableStream with byte source: respondWithNewView() throws if the supplied view's buffer is zero-length (in the closed state)",
      // TODO(conform): The spec expects this to be a RangeError
      "ReadableStream with byte source: respondWithNewView() throws if the supplied view's buffer has a different length (in the closed state)",
      // TODO(conform): We currently do not throw here since reading causes the
      // view here to be zero length, which is allowed when the stream is closed.
      //assert_throws_js(TypeError, () => c.byobRequest.respondWithNewView(view));
      "ReadableStream with byte source: enqueue() throws if the BYOB request's buffer has been detached (in the closed state)",
    ],
  },
  'readable-byte-streams/construct-byob-request.any.js': {},
  'readable-byte-streams/enqueue-with-detached-buffer.any.js': {},
  'readable-byte-streams/general.any.js': {
    comment: 'See individual comments',
    expectedFailures: [
      // TODO(conform): The spec expects that errors thrown synchronously in the start
      // algorithm should cause the ReadableStream constructor to throw. We currently
      // don't do that but we do error the stream.
      // assert_throws_js(Error, () => new ReadableStream({ start() { throw new Error(); }, type:'bytes' }),
      //     'start() can throw an exception with type: bytes');
      'ReadableStream with byte source: start() throws an exception',
      // TODO(conform): The spec expects pull not to have been called yet, but as an optimization
      // since start is not provided we treat is synchronously and pull proactively, making this
      // next check invalid.
      // assert_equals(pullCount, 0, 'No pull as start() just finished and is not yet reflected to the state of the stream');
      'ReadableStream with byte source: Automatic pull() after start()',
      // TODO(conform): The spec expects pull not to have been called yet, but as an optimization
      // since start is not provided we treat is synchronously and pull proactively, making this
      // next check invalid.
      //assert_equals(pullCount, 0, 'No pull as start() just finished and is not yet reflected to the state of the stream');
      'ReadableStream with byte source: Automatic pull() after start() and read()',
      'ReadableStream with byte source: autoAllocateChunkSize',
      'ReadableStream with byte source: Automatic pull() after start() and read(view)',
      'ReadableStream with byte source: Respond to pull() by enqueue() asynchronously',
      'ReadableStream with byte source: Respond to multiple pull() by separate enqueue()',
      'ReadableStream with byte source: read() twice, then enqueue() twice',
      // TODO(conform): The spec would not expect pull to be called because of the close,
      // but because our implementation calls pull immediately on the first read, we
      // differ slightly here.
      // assert_unreached("pull() should not have been called");
      // TODO(conform): The spec would allow the byobRequest to still be used here, but
      // our implementation throws when accessed after close.
      // controller.byobRequest.respond(0);
      'ReadableStream with byte source: Multiple read(view), close() and respond()',
      'ReadableStream constructor should not accept a strategy with a size defined if type is "bytes"',
      'ReadableStream with byte source: enqueue(), getReader(), then read()',
      // TODO(conform): This is a case where our implementation intentionally
      // diverges from the spec due to the tee backpressure implementation.
      // Specifically, the input view is a Uint16Array with one element --
      // meaning it expects us to provide 2 bytes. The enqueue() only gives
      // it one byte. Because of how we handle these internally, the read will
      // not be fulfilled until another byte is provided, but the byobRequest
      // still is invalidated. In the standard, the byobRequest would still
      // be valid here.
      //
      // Generally speaking, in our implementation, using enqueue() and byobRequest
      // together is not something that should be done.
      'ReadableStream with byte source: cancel() with partially filled pending pull() request',
      "ReadableStream with byte source: Push source that doesn't understand pull signal",
      'ReadableStream with byte source: enqueue() with Uint16Array, getReader(), then read()',
      // TODO(conform): Our implementation ends up immediately calling pull
      // when the read() is called, before the cancel() is able to run. The
      // spec expects the cancel to happen first.
      //assert_unreached("pull should not have been called");
      // TODO(conform): The spec expects the result.value here to be undefined since the read
      // is canceled. Our impl returns an empty ArrayBuffer...
      //assert_equals(result.value, undefined, 'result.value');
      'ReadableStream with byte source: getReader(), read(view), then cancel()',
      'ReadableStream with byte source: read(view) with Uint32Array, then fill it by multiple enqueue() calls',
      'ReadableStream with byte source: enqueue(), read(view) partially, then read()',
      'ReadableStream with byte source: read(view), then respond() and close() in pull()',
      // TODO(conform): The spec expects the read to fail here. Instead, we end up cancelling
      // it with a zero-length result, with the subsequent read marked as done.
      'ReadableStream with byte source: read(view) with Uint16Array on close()-d stream with 1 byte enqueue()-d must fail',
      // TODO(conform): Per the spec, desiredSize should be zero here
      // but since we are handling the backpressure a bit differently
      // it won't be zero until the actual read is resolved.
      //desiredSize = controller.desiredSize;
      'ReadableStream with byte source: enqueue() 3 byte, getReader(), then read(view) with 2-element Uint16Array',
      'ReadableStream with byte source: Throwing in pull in response to read() must be ignored if the stream is errored in it',
      'ReadableStream with byte source: Throwing in pull function must error the stream',
      // TODO(conform): We handle things a bit differently here from the spec. The spec
      // would have the enqueue() complete replace the byobRequest.view while we use it
      // and fill it with the data from the enqueue. This means the following check is
      // not valid in our implementation.
      // assert_array_equals([...new Uint8Array(view1.buffer)], [1, 2, 3], 'first result.value.buffer');
      'ReadableStream with byte source: enqueue() discards auto-allocated BYOB request',
      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: releaseLock() with pending read(view), read(view) on second reader, respond()',

      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: releaseLock() with pending read(view), read(view) on second reader with 1 element Uint16Array, respond(1)',

      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: releaseLock() with pending read(view), read(view) on second reader with 2 element Uint8Array, respond(3)',

      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: releaseLock() with pending read(view), read(view) on second reader, respondWithNewView()',

      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: autoAllocateChunkSize, releaseLock() with pending read(), read() on second reader, respond()',

      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: autoAllocateChunkSize, releaseLock() with pending read(), read() on second reader, enqueue()',

      // TODO(conform): Calling releaseLock() should cancel the pending reads. It currently does not.
      'ReadableStream with byte source: autoAllocateChunkSize, releaseLock() with pending read(), read(view) on second reader, respond()',
      // TODO(conform): The spec allows a byob read to be fulfilled incrementally over multiple
      // respond calls, we currently do not.
      'ReadableStream with byte source: read(view) with 1 element Uint16Array, respond(1), releaseLock(), read(view) on second reader with 1 element Uint16Array, respond(1)',
      // TODO(conform): The spec allows a byob read to be fulfilled incrementally over multiple
      // respond calls, we currently do not.
      'ReadableStream with byte source: read(view) with 1 element Uint16Array, respond(1), releaseLock(), read() on second reader, enqueue()',
      // TODO: investigate this
      'ReadableStream with byte source: A stream must be errored if close()-d before fulfilling read(view) with Uint16Array',
      // TODO: investigate this
      'ReadableStream with byte source: Multiple read(view), big enqueue()',
      // TODO: investigate this
      'ReadableStream with byte source: Multiple read(view) and multiple enqueue()',
    ],
  },
  'readable-byte-streams/non-transferable-buffers.any.js': {},
  'readable-byte-streams/patched-global.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [
      'Patched then() sees byobRequest after filling all pending pull-into descriptors',
    ],
    runInGlobalScope: true,
  },
  'readable-byte-streams/read-min.any.js': {
    comment: 'A hanging Promise was canceled.',
    disabledTests: true,
  },
  'readable-byte-streams/respond-after-enqueue.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'byobRequest.respond() after enqueue() with double read should not crash',
    ],
  },
  'readable-byte-streams/tee.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'ReadableStream teeing with byte source: pull with default reader, then pull with BYOB reader',
      'ReadableStream teeing with byte source: chunks should be cloned for each branch',
      'ReadableStream teeing with byte source: reading an array with a byte offset should clone correctly',
      'ReadableStream teeing with byte source: chunks for BYOB requests from branch 1 should be cloned to branch 2',
      'ReadableStream teeing with byte source: canceling both branches should aggregate the cancel reasons into an array',
      'ReadableStream teeing with byte source: canceling both branches in reverse order should aggregate the cancel reasons into an array',
      'ReadableStream teeing with byte source: pull with BYOB reader, then pull with default reader',
      'ReadableStream teeing with byte source: failing to cancel the original stream should cause cancel() to reject on branches',
      'ReadableStream teeing with byte source: should be able to read one branch to the end without affecting the other',
      'ReadableStream teeing with byte source: canceling branch1 should not impact branch2',
      'ReadableStream teeing with byte source: canceling branch2 should not impact branch1',
      'ReadableStream teeing with byte source: canceling both branches in sequence with delay',
      'ReadableStream teeing with byte source: failing to cancel when canceling both branches in sequence with delay',
      'ReadableStream teeing with byte source: enqueue() and close() while both branches are pulling',
      'ReadableStream teeing with byte source: stops pulling when original stream errors while both branches are reading',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch1, cancel branch2',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch2, cancel branch1',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch2, enqueue to branch1',
      'ReadableStream teeing with byte source: read from branch1 and branch2, cancel branch1, respond to branch2',
      'ReadableStream teeing with byte source: read from branch1 with default reader, then close while branch2 has pending BYOB read',
      'ReadableStream teeing with byte source: read from branch2 with default reader, then close while branch1 has pending BYOB read',
    ],
  },
  'readable-byte-streams/templated.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'ReadableStream with byte source (empty): calling getReader with invalid arguments should throw appropriate errors',
      'ReadableStream with byte source (empty) BYOB reader: canceling via the reader should cause the reader to act closed',
    ],
  },

  'readable-streams/async-iterator.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Async iterator instances should have the correct list of properties',
      'return(); next() [no awaiting]',
      'return(); return() [no awaiting]',
      'return(); next() with delayed cancel() [no awaiting]',
      'next() that succeeds; return()',
      'next() that succeeds; next() that reports an error(); next() [no awaiting]',
      'next() that succeeds; next() that reports an error(); return() [no awaiting]',
      'next() that succeeds; return() [no awaiting]',
      'next() that succeeds; next() that reports an error; next()',
      'next() that succeeds; next() that reports an error(); return()',
    ],
  },
  'readable-streams/bad-strategies.any.js': {
    comment: 'See individual comments',
    expectedFailures: [
      // TODO(conform): While we do error the stream, the spec expects us to throw the error
      // thrown in the size() function here. We currently do not.
      'Readable stream: strategy.size errors the stream and then throws',
      // TODO(conform): While we do error the stream, the spec expects us to throw the error
      // thrown in the size() function here. We currently do not.
      'Readable stream: strategy.size errors the stream and then returns Infinity',
      // TODO(conform): The spec expects this to be a TypeError, we currently throw
      // a RangeError instead
      'Readable stream: invalid strategy.highWaterMark',
      // TODO(conform): We currently do not error when the size function returns the wrong
      // value. The spec expects us to. We do properly error the strea
      'Readable stream: invalid strategy.size return value',
      'Readable stream: invalid strategy.size return value when pulling',
    ],
  },
  'readable-streams/bad-underlying-sources.any.js': {
    comment: 'See individual comments',
    disabledTests: [
      // TODO(conform): The spec expects pull to be called twice when the stream is created and
      // a single read happens. We currently only call it once in this case, so we have to read
      // again to trigger the error case.
      'Underlying source pull: throwing method (second pull)',
      // TODO(conform): The spec expects pull() to be called twice when the stream is
      // constructed and the first read occurs, we currently only call it once, so to
      // trigger the error, we perform a read again.
      'read should not error if it dequeues and pull() throws',
    ],
    expectedFailures: [
      // TODO(conform): If the start function throws synchronously, the constructor
      // should throw, per the spec. Currently we only error the stream and do not
      // throw synchronously here.
      'Underlying source start: throwing method',
      // TODO(conform): The spec says that a second call to error should be a non-op.
      // We currently treat it an an error.
      'Underlying source: calling error twice should not throw',
      // TODO(conform): The spec says that calling error() after close() should be a non-op.
      // We currently treat it as an error.
      'Underlying source: calling error after close should not throw',
    ],
  },
  'readable-streams/cancel.any.js': {
    comment: 'See detailed explanation in comments',
    disabledTests: [
      // underlyingSource is converted in prose in the method body, whereas queuingStrategy is done at the IDL layer.
      // So the queuingStrategy exception should be encountered first.
      // TODO(conform): We currently handle these differently and end up throwing error1 instead.
      'ReadableStream cancellation: underlyingSource.cancel() should called, even with pending pull',
    ],
  },
  'readable-streams/constructor.any.js': {
    comment: 'They want us to validate the args and throw in a different order',
    expectedFailures:
      process.platform === 'win32'
        ? []
        : [
            'underlyingSource argument should be converted after queuingStrategy argument',
          ],
  },
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
  'readable-streams/default-reader.any.js': {
    comment: 'See individual comments',
    expectedFailures: [
      // TODO(conform): When releaseLock() is called, the spec expects the readers original
      // closed promise to be replaced. The original one should be resolved, but the new
      // one should be rejected. We currently do not replace the closed promise in this case.
      'closed is replaced when stream closes and reader releases its lock',
      // TODO(conform): When releaseLock() is called, the spec expects the readers original
      // closed promise to be replaced. In this case, the original one should reject with
      // theError, while the second should reject indicating that it was acquired after
      // releasing the lock. We currently do not replace the closed promise in this case.
      // assert_not_equals(promise1, promise2, '.closed should be replaced');
      'closed is replaced when stream errors and reader releases its lock',
      // TODO(conform): The spec allows error to be called with no argument at all, treating
      // it as undefined, currently we require that undefined is passed explicitly.
      'ReadableStreamDefaultReader closed promise should be rejected with undefined if that is the error',
      // TODO(conform): The spec expects this to be a TypeError, not a RangeError
      'getReader() should call ToString() on mode',
    ],
  },
  'readable-streams/floating-point-total-queue-size.any.js': {
    comment: 'Queue size needs to use double math',
    expectedFailures: [
      'Floating point arithmetic must manifest near NUMBER.MAX_SAFE_INTEGER (total ends up positive)',
      'Floating point arithmetic must manifest near 0 (total ends up positive, but clamped)',
      'Floating point arithmetic must manifest near 0 (total ends up positive, and not clamped)',
      'Floating point arithmetic must manifest near 0 (total ends up zero)',
    ],
  },
  'readable-streams/from.any.js': {
    comment: 'See comments on tests',
    disabledTests: [
      // A hanging promise was cancelled
      'ReadableStream.from: cancel() resolves when return() method is missing',
      'ReadableStream.from: cancel() rejects when return() rejects',
      'ReadableStream.from: cancel() rejects when return() fulfills with a non-object',
      // Heap use-after-free
      'ReadableStream.from: reader.cancel() inside return()',
      'ReadableStream.from: reader.cancel() inside next()',
    ],
    expectedFailures: [
      // To be investigated
      'ReadableStream.from: cancel() rejects when return() is not a method',
      'ReadableStream.from accepts a string',
      'ReadableStream.from: cancel() rejects when return() throws synchronously',
      'ReadableStream.from accepts an array of promises',
      'ReadableStream.from accepts a sync iterable of promises',
      'ReadableStream.from ignores a null @@asyncIterator',
      'ReadableStream.from: cancelling the returned stream calls and awaits return()',
    ],
  },
  'readable-streams/garbage-collection.any.js': {
    comment: 'See comments on individual tests',
    disabledTests: [
      // A hanging promise was cancelled
      'ReadableStream closed promise should reject even if stream and reader JS references are lost',
      'Garbage-collecting a ReadableStreamDefaultReader should not unlock its stream',
      'A ReadableStream and its reader should not be garbage collected while there is a read promise pending',
    ],
    expectedFailures: [
      // Failed to execute 'error' on 'ReadableStreamDefaultController': parameter 1 is not of type 'Value'
      'ReadableStreamController methods should continue working properly when scripts lose their reference to the readable stream',
    ],
  },
  'readable-streams/general.any.js': {
    comment: 'See individual comments',
    expectedFailures: [
      // TODO(conform): We currently allow `new ReadableStream(null)`...
      "ReadableStream can't be constructed with garbage",
      // TODO(conform): We currently allow the empty type value
      "ReadableStream can't be constructed with an invalid type",
      // TODO(conform): The spec expects a TypeError here, not a RangeError
      'default ReadableStream getReader() should only accept mode:undefined',
      // TODO(conform): The spec expects us to call pull an extra time here despite. [Despite what? -NP]
      'ReadableStream: should pull after start, and after every read',
      // TODO(conform): The standard generally anticipates that the closed
      // promise rejection will happen before the read promise rejection.
      // We don't follow that ordering currently.
      'ReadableStream: if pull rejects, it should error the stream',
      // TODO(conform): The read above is fulfilled by the c.enqueue() in the start algorithm.
      // The spec expects us to call pull() again to prime the queue again for the next read
      // but we currently do not. We only pull when we get another read
      'ReadableStream: should only call pull once on a non-empty stream read from after start fulfills',
      // TODO(conform): The spec expects us to call pull twice even tho we've only had a
      // single read. We currently wait to pull again only when another read occurs.
      'ReadableStream: should call pull in reaction to read()ing the last chunk, if not draining',
      // TODO(conform): The spec expects us to call pull twice even tho we've only had a single
      // read. We currently only call it when we have an actual read to fulfill.
      "ReadableStream: should not call pull until the previous pull call's promise fulfills",
    ],
  },
  'readable-streams/owning-type-message-port.any.js': {
    comment: 'Enable once MessageChannel/MessagePort is implemented',
    expectedFailures: [
      'Transferred MessageChannel works as expected',
      'Second branch of owning ReadableStream tee should end up into errors with transfer only values',
    ],
  },
  'readable-streams/owning-type-video-frame.any.js': {
    comment: 'VideoFrame is not implemented',
    expectedFailures: [
      'ReadableStream of type owning should close serialized chunks',
      'ReadableStream of type owning should transfer JS chunks with transferred values',
      'ReadableStream of type owning should error when trying to enqueue not serializable values',
      'ReadableStream of type owning should clone serializable objects when teeing',
      'ReadableStream of type owning should clone JS Objects with serializables when teeing',
    ],
  },
  'readable-streams/owning-type.any.js': {
    comment: "Type 'owning' is not implemented",
    expectedFailures: [
      'ReadableStream can be constructed with owning type',
      'ReadableStream of type owning should call start with a ReadableStreamDefaultController',
      'ReadableStream should be able to call enqueue with an empty transfer list',
      'ReadableStream of type owning should transfer enqueued chunks',
    ],
  },
  'readable-streams/patched-global.any.js': {
    runInGlobalScope: true,
  },
  'readable-streams/read-task-handling.window.js': {
    comment: 'document is not defined',
    disabledTests: true,
  },
  'readable-streams/reentrant-strategies.any.js': {
    comment: 'See individual comments',
    expectedFailures: [
      // TODO(conform): In this edge case, the spec expects the chunk to still be successfully
      // enqueued even tho the stream gets closed. We currently throw in this case. Whether or
      // not that ultimately matters is something up for debate since in either case the chunk
      // cannot be read.
      'close() inside size() should not crash',
      // TODO(conform): Like the case above, the spec expects us to still successfully enqueue
      // the chunk here. Unlike the previous case, we should still be able to read this chunk
      // so this is a case we should definitely support.
      'close request inside size() should work',
      // TODO(conform): The spec expects us to still enqueue the value but the read() should still
      // reject.
      'error() inside size() should work',
      // TODO(conform): The spec expects the enqueue() to still go through without an error
      // here but we currently throw an error here.
      'cancel() inside size() should work',
      // TODO(conform): We currently fail this test. Need to investigate why
      'read() inside of size() should behave as expected',
    ],
  },
  'readable-streams/tee.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'ReadableStream teeing: errors in the source should propagate to both branches',
      'ReadableStreamTee should only pull enough to fill the emptiest queue',
      'ReadableStreamTee stops pulling when original stream errors while branch 1 is reading',
      'ReadableStreamTee stops pulling when original stream errors while branch 2 is reading',
      'ReadableStreamTee stops pulling when original stream errors while both branches are reading',
      'ReadableStream teeing: canceling both branches should aggregate the cancel reasons into an array',
      'ReadableStream teeing: canceling both branches in reverse order should aggregate the cancel reasons into an array',
      'ReadableStream teeing: failing to cancel the original stream should cause cancel() to reject on branches',
      'ReadableStream teeing: failing to cancel when canceling both branches in sequence with delay',
      'ReadableStreamTee should not pull more chunks than can fit in the branch queue',
    ],
  },
  'readable-streams/templated.any.js': {
    comment: 'To be investigated',
    disabledTests: [
      // TODO(soon): This test appears to mess up the state of the workerd test case itself. Investigate why.
      'ReadableStream reader (closed via cancel after getting reader): closed should fulfill with undefined',
    ],
    expectedFailures: [
      'ReadableStream (empty): calling getReader with invalid arguments should throw appropriate errors',
      'ReadableStream reader (closed before getting reader): releasing the lock should cause closed to reject and change identity',
      'ReadableStream reader (closed after getting reader): releasing the lock should cause closed to reject and change identity',
      'ReadableStream reader (closed via cancel after getting reader): releasing the lock should cause closed to reject and change identity',
      'ReadableStream (errored via returning a rejected promise in start) reader: releasing the lock should cause closed to reject and change identity',
      'ReadableStream reader (errored before getting reader): releasing the lock should cause closed to reject and change identity',
      'ReadableStream reader (errored after getting reader): releasing the lock should cause closed to reject and change identity',
    ],
  },

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

  'transform-streams/backpressure.any.js': {
    comment: 'A hanging Promise was canceled.',
    disabledTests: true,
  },
  'transform-streams/cancel.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'readable.cancel() and a parallel writable.close() should reject if a transformer.cancel() calls controller.error()',
      'closing the writable side should reject if a parallel transformer.cancel() throws',
      'writable.abort() and readable.cancel() should reject if a transformer.cancel() calls controller.error()',
      'readable.cancel() should not call cancel() again when already called from writable.abort()',
      'writable.close() should not call flush() when cancel() is already called from readable.cancel()',
      'writable.abort() should not call cancel() again when already called from readable.cancel()',
      'readable.cancel() should not call cancel() when flush() is already called from writable.close()',
    ],
  },
  'transform-streams/errors.any.js': {
    comment: 'To be investigated',
    disabledTests: [
      // TODO(soon): This test appears to mess up the state of the workerd test case itself. Investigate why.
      'an exception from transform() should error the stream if terminate has been requested but not completed',
    ],
    expectedFailures: [
      'when controller.error is followed by a rejection, the error reason should come from controller.error',
      'TransformStream constructor should throw when start does',
      'when strategy.size throws inside start(), the constructor should throw the same error',
      'when strategy.size calls controller.error() then throws, the constructor should throw the first error',
      'it should be possible to error the readable between close requested and complete',
      'controller.error() should do nothing after a transformer method has thrown an exception',
      'controller.error() should do nothing the second time it is called',
      'abort should set the close reason for the writable when it happens before cancel during start, and cancel should reject',
      'controller.error() should close writable immediately after readable.cancel()',
      'abort should set the close reason for the writable when it happens before cancel during underlying sink write, but cancel should still succeed',
      'erroring during write with backpressure should result in the write failing',
    ],
  },
  'transform-streams/flush.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'error() during flush should cause writer.close() to reject',
    ],
  },
  'transform-streams/general.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'it should be possible to call transform() synchronously',
      'specifying a defined readableType should throw',
      'specifying a defined writableType should throw',
      'terminate() should abort writable immediately after readable.cancel()',
    ],
  },
  'transform-streams/invalid-realm.tentative.window.js': {
    comment: 'document is not defined',
    expectedFailures: [
      'TransformStream: write in detached realm should succeed',
    ],
  },
  'transform-streams/lipfuzz.any.js': {},
  'transform-streams/patched-global.any.js': {
    runInGlobalScope: true,
  },
  'transform-streams/properties.any.js': {
    comment: 'The value cannot be converted because it is not an integer.',
    expectedFailures: [
      'transformer method start should be called with the right number of arguments',
      "transformer method start should be called even when it's located on the prototype chain",
      'transformer method transform should be called with the right number of arguments',
      "transformer method transform should be called even when it's located on the prototype chain",
      'transformer method flush should be called with the right number of arguments',
      "transformer method flush should be called even when it's located on the prototype chain",
    ],
  },
  'transform-streams/reentrant-strategies.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'enqueue() inside size() should work',
      'terminate() inside size() should work',
      'error() inside size() should work',
      'readable cancel() inside size() should work',
      'read() inside of size() should work',
      'writer.write() inside size() should work',
      'synchronous writer.write() inside size() should work',
      'writer.close() inside size() should work',
      'writer.abort() inside size() should work',
    ],
  },
  'transform-streams/strategies.any.js': {
    comment: 'To be investigated',
    disabledTests: [
      // TODO(soon): This test appears to mess up the state of the workerd test case itself. Investigate why.
      'default readable strategy should be equivalent to { highWaterMark: 0 }',
    ],
    expectedFailures: [
      'writable should have the correct size() function',
      'a RangeError should be thrown for an invalid highWaterMark',
      'a bad readableStrategy size function should error the stream on enqueue even when transformer.transform() catches the exception',
      'a bad readableStrategy size function should cause writer.write() to reject on an identity transform',
    ],
  },
  'transform-streams/terminate.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'controller.error() after controller.terminate() with queued chunk should error the readable',
      'controller.error() after controller.terminate() without queued chunk should do nothing',
      'controller.terminate() inside flush() should not prevent writer.close() from succeeding',
    ],
  },

  'writable-streams/aborting.any.js': {
    comment: 'This is mostly nitpickiness about the type of error',
    expectedFailures: [
      "Aborting a WritableStream before it starts should cause the writer's unsettled ready promise to reject",
      "WritableStream if sink's abort throws, the promise returned by multiple writer.abort()s is the same and rejects",
      'when calling abort() twice on the same stream, both should give the same promise that fulfills with undefined',
      'the abort signal is signalled synchronously - write',
      'Aborting a WritableStream causes any outstanding write() promises to be rejected with the reason supplied',
      'Aborting a WritableStream puts it in an errored state with the error passed to abort()',
      'if a writer is created for a stream with a pending abort, its ready should be rejected with the abort error',
      'sink abort() should not be called if stream was erroring due to bad strategy before abort() was called',
      "WritableStream if sink's abort throws, for an abort performed during a write, the promise returned by ws.abort() rejects",
      'writer.abort() while there is an in-flight write, and then finish the write with rejection',
      'writer.abort(), controller.error() while there is an in-flight write, and then finish the write',
      'writer.abort(), controller.error() while there is an in-flight close, and then finish the close',
      'controller.error(), writer.abort() while there is an in-flight write, and then finish the write',
      'controller.error(), writer.abort() while there is an in-flight close, and then finish the close',
    ],
  },
  'writable-streams/bad-strategies.any.js': {
    comment: 'We have TypeError, they want RangeError',
    expectedFailures: [
      'Writable stream: invalid strategy.highWaterMark',
      'Writable stream: invalid strategy.size return value',
    ],
  },
  'writable-streams/bad-underlying-sinks.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'start: errors in start cause WritableStream constructor to throw',
      'write: returning a rejected promise (second write) should cause writer write() and ready to reject',
      'write: returning a promise that becomes rejected after the writer write() should cause writer write() and ready to reject',
    ],
  },
  'writable-streams/byte-length-queuing-strategy.any.js': {
    comment:
      'TypeError: The value cannot be converted because it is not an integer.',
    expectedFailures: [
      'Closing a writable stream with in-flight writes below the high water mark delays the close call properly',
    ],
  },
  'writable-streams/close.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'when close is called on a WritableStream in waiting state, ready promise should be fulfilled',
      'releaseLock() should not change the result of sync close()',
      'close() on an errored stream should reject',
    ],
  },
  'writable-streams/constructor.any.js': {
    comment: 'These are mostly about validation of params',
    expectedFailures:
      process.platform === 'win32'
        ? [
            'controller argument should be passed to start method',
            'WritableStream should be writable and ready should fulfill immediately if the strategy does not apply backpressure',
            "WritableStream can't be constructed with a defined type",
          ]
        : [
            'controller argument should be passed to start method',
            'WritableStream should be writable and ready should fulfill immediately if the strategy does not apply backpressure',
            "WritableStream can't be constructed with a defined type",
            'underlyingSink argument should be converted after queuingStrategy argument',
          ],
  },
  'writable-streams/count-queuing-strategy.any.js': {},
  'writable-streams/crashtests/garbage-collection.any.js': {},
  'writable-streams/error.any.js': {},
  'writable-streams/floating-point-total-queue-size.any.js': {
    comment: 'Seems we should be using a double for queue size',
    expectedFailures: [
      'Floating point arithmetic must manifest near NUMBER.MAX_SAFE_INTEGER (total ends up positive)',
      'Floating point arithmetic must manifest near 0 (total ends up positive, but clamped)',
      'Floating point arithmetic must manifest near 0 (total ends up positive, and not clamped)',
      'Floating point arithmetic must manifest near 0 (total ends up zero)',
    ],
  },
  'writable-streams/garbage-collection.any.js': {},
  'writable-streams/general.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'desiredSize on a writer for an errored stream',
      "WritableStream's strategy.size should not be called as a method",
      'closed and ready on a released writer',
      'ready promise should fire before closed on releaseLock',
    ],
  },
  'writable-streams/properties.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'sink method write should be called with the right number of arguments',
      "sink method write should be called even when it's located on the prototype chain",
    ],
  },
  'writable-streams/reentrant-strategy.any.js': {
    comment: 'A hanging Promise was canceled.',
    disabledTests: true,
  },
  'writable-streams/start.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      "underlying sink's write or close should not be called if start throws",
      'when start() rejects, writer promises should reject in standard order',
    ],
  },
  'writable-streams/write.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'write() on a stream with HWM 0 should not cause the ready Promise to resolve',
      'WritableStream should transition to waiting until write is acknowledged',
      "when sink's write throws an error, the stream should become errored and the promise should reject",
    ],
  },
} satisfies TestRunnerConfig;
