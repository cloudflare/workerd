// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok, rejects } from 'node:assert';

// Test pipeThrough from JavaScript readable to internal writable
export const pipeThroughJsToInternal = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = [enc.encode('hello'), enc.encode('there'), 'hello'];
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    const output = [];
    let errored = false;
    try {
      for await (const chunk of readable) {
        output.push(dec.decode(chunk));
      }
    } catch (err) {
      // The 'hello' string at the end of chunks will cause an error to be thrown.
      strictEqual(
        err.message,
        'This WritableStream only supports writing byte types.'
      );
      errored = true;
    }

    strictEqual(output[0], 'hello');
    strictEqual(output[1], 'there');
    strictEqual(output.length, 2);
    ok(errored);
  },
};

// Test pipeThrough error in JS Readable aborts Internal Writable when preventAbort = false
export const pipeThroughJsToInternalErroredSource = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    ok(transform.writable.locked);

    const reader = readable.getReader();

    await rejects(reader.read(), { message: 'boom' });

    ok(!transform.writable.locked);

    // Attempts to use the writable from here on will fail with the same error.
    const writer = transform.writable.getWriter();
    await rejects(writer.write(enc.encode('hello')), { message: 'boom' });
  },
};

// Test pipeTo error in JS Readable aborts Internal Writable when preventAbort = false
export const pipeToJsToInternalErroredSource = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const { readable, writable } = new IdentityTransformStream();
    const pipe = rs.pipeTo(writable);

    ok(writable.locked);

    const reader = readable.getReader();

    await rejects(reader.read(), { message: 'boom' });

    ok(!writable.locked);

    // Attempts to use the writable from here on will fail with the same error.
    const writer = writable.getWriter();
    await rejects(writer.write(enc.encode('hello')), { message: 'boom' });

    await rejects(pipe, { message: 'boom' });
  },
};

// Test pipeThrough from internal readable to JS writable
export const pipeThroughInternalToJs = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch('http://test/stream');
    const dec = new TextDecoder();

    const output = [];
    const ws = new WritableStream({
      write(chunk) {
        output.push(dec.decode(chunk));
      },
    });

    const ts = new TransformStream();
    const pipePromise = response.body.pipeThrough(ts).pipeTo(ws);

    await pipePromise;

    strictEqual(output.join(''), 'hello world '.repeat(100));
  },
};

// Test pipeTo from internal readable to JS writable
export const pipeToInternalToJs = {
  async test(_ctrl, env) {
    const response = await env.SERVICE.fetch('http://test/stream');
    const dec = new TextDecoder();

    const output = [];
    const ws = new WritableStream({
      write(chunk) {
        output.push(dec.decode(chunk));
      },
    });

    await response.body.pipeTo(ws);

    strictEqual(output.join(''), 'hello world '.repeat(100));
  },
};

// Test pipeThrough from JS readable to JS writable
export const pipeThroughJsToJs = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    const chunks = [enc.encode('hello'), enc.encode('there')];
    const rs = new ReadableStream({
      pull(c) {
        const chunk = chunks.shift();
        if (chunk) {
          c.enqueue(chunk);
        } else {
          c.close();
        }
      },
    });

    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(dec.decode(chunk).toUpperCase());
      },
    });

    const output = [];
    const ws = new WritableStream({
      write(chunk) {
        output.push(chunk);
      },
    });

    await rs.pipeThrough(ts).pipeTo(ws);

    strictEqual(output.join(''), 'HELLOTHERE');
  },
};

// NOTE: simple-pipeto-js-to-internal from pipe.ew-test is NOT PORTED
// This test requires pipeTo to reject while consuming readable side, which causes
// an unhandled promise rejection error in workerd's test environment before the
// error can be caught. The test still exists in the ew-test harness.

// Test pipeThrough error in JS Readable does not abort Internal Writable when preventAbort = true
export const pipeThroughJsToInternalErroredSourcePreventAbort = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const transform = new IdentityTransformStream();
    const rs = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const readable = rs.pipeThrough(transform, { preventAbort: true });

    let reader = readable.getReader();

    ok(rs.locked);
    ok(transform.writable.locked);
    ok(transform.readable.locked);

    // Wait a tick to allow the event loop to advance so that the error
    // in the ReadableStream pull is processed.
    await scheduler.wait(1);

    reader.releaseLock();
    ok(!rs.locked);
    ok(!transform.readable.locked);
    ok(!transform.writable.locked);

    // We can still use the transform's readable and writable here.
    const writer = transform.writable.getWriter();
    reader = transform.readable.getReader();

    await Promise.all([writer.write(enc.encode('hello')), reader.read()]);
  },
};

// Test pipeTo error in JS Readable does not abort Internal Writable when preventAbort = true
export const pipeToJsToInternalErroredSourcePreventAbort = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const { writable, readable } = new IdentityTransformStream();
    const rs = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const pipe = rs.pipeTo(writable, { preventAbort: true });

    let reader = readable.getReader();

    ok(rs.locked);
    ok(writable.locked);
    ok(readable.locked);

    // The pipe promise should be rejected here but the WritableStream
    // destination should still be usable.
    await rejects(pipe, { message: 'boom' });

    reader.releaseLock();
    ok(!rs.locked);
    ok(!readable.locked);
    ok(!writable.locked);

    // We can still use the transform's readable and writable here.
    const writer = writable.getWriter();
    reader = readable.getReader();
    writer.write(enc.encode('hello'));
    writer.close();
    const result = await reader.read();
    strictEqual(dec.decode(result.value), 'hello');
  },
};

// Test pipeThrough error in Writable cancels Readable when preventCancel = false
export const pipeThroughJsToInternalErroredDest = {
  async test() {
    const enc = new TextEncoder();
    const transform = new IdentityTransformStream();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const readable = rs.pipeThrough(transform);

    const reader = readable.getReader();

    ok(rs.locked);
    ok(transform.writable.locked);

    reader.cancel(new Error('boom'));
    reader.releaseLock();

    // Wait a tick to allow the event loop to turn over to process the abort
    await scheduler.wait(1);

    ok(!rs.locked);
    ok(!transform.readable.locked);
    ok(!transform.writable.locked);

    // Our JavaScript ReadableStream should no longer be usable.
    const reader2 = rs.getReader();
    await rejects(reader2.read(), { message: 'boom' });
  },
};

// Test pipeTo error in Writable cancels Readable when preventCancel = false
export const pipeToJsToInternalErroredDest = {
  async test() {
    const enc = new TextEncoder();
    const { readable, writable } = new IdentityTransformStream();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const pipe = rs.pipeTo(writable);

    const reader = readable.getReader();

    ok(rs.locked);
    ok(writable.locked);

    reader.cancel(new Error('boom'));
    reader.releaseLock();

    await rejects(pipe, { message: 'boom' });

    ok(!rs.locked);
    ok(!readable.locked);
    ok(!writable.locked);

    // Our JavaScript ReadableStream should no longer be usable.
    const reader2 = rs.getReader();
    await rejects(reader2.read(), { message: 'boom' });
  },
};

// NOTE: pipethrough-js-to-internal-errored-dest-prevent from pipe.ew-test is NOT PORTED
// This test causes an unhandled promise rejection error in workerd's test environment
// before the error can be caught. The test still exists in the ew-test harness.

// NOTE: pipeto-js-to-internal-errored-dest-prevent from pipe.ew-test is NOT PORTED
// This test causes an unhandled promise rejection error in workerd's test environment
// before the error can be caught. The test still exists in the ew-test harness.

// Test closing Readable closes Writable when preventClose = false
export const pipeThroughJsToInternalCloses = {
  async test() {
    const enc = new TextEncoder();
    const chunks = [enc.encode('hello'), enc.encode('there')];
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    for await (const chunk of readable) {
      // consume all chunks
    }

    // The writable should be closed and locked by the pipe
    try {
      const writer = transform.writable.getWriter();
      throw new Error('should not reach here');
    } catch (err) {
      strictEqual(
        err.message,
        'This WritableStream is currently locked to a writer.'
      );
    }
  },
};

// Test closing Readable does not close Writable when preventClose = true
export const pipeThroughJsToInternalPreventClose = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform, { preventClose: true });

    // Because the internal TransformStream here won't resolve the write
    // promises until a read has been performed, we have to read, then
    // wait a turn of the event loop before we can check that the writer
    // is still in the correct state.
    const reader = readable.getReader();
    await reader.read();

    await scheduler.wait(1);

    // The WritableStream should not be closed and still usable.
    const writer = transform.writable.getWriter();
    writer.write(enc.encode('there'));
    const read = await reader.read();
    strictEqual(dec.decode(read.value), 'there');
  },
};

// Test pipeThrough with BYOB ReadableStream works
export const pipeThroughJsByobToInternal = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = [enc.encode('hello'), enc.encode('there')];
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    const output = [];
    for await (const chunk of readable) {
      output.push(dec.decode(chunk));
    }

    strictEqual(output[0], 'hello');
    strictEqual(output[1], 'there');
  },
};

// Test pipeTo with BYOB ReadableStream works
export const pipeToJsByobToInternal = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = [enc.encode('hello'), enc.encode('there')];
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const { readable, writable } = new IdentityTransformStream();
    rs.pipeTo(writable);

    const output = [];
    for await (const chunk of readable) {
      output.push(dec.decode(chunk));
    }

    strictEqual(output[0], 'hello');
    strictEqual(output[1], 'there');
  },
};

// Test simple pipeTo from internal readable to JavaScript writable
export const pipeToInternalToJsSimple = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    const { readable, writable } = new IdentityTransformStream();

    const chunks = [];
    const ws = new WritableStream({
      write(chunk) {
        chunks.push(chunk);
      },
    });

    const pipe = readable.pipeTo(ws);

    const writer = writable.getWriter();
    writer.write(enc.encode('hello'));
    writer.write(enc.encode('there'));
    writer.close();

    await pipe;

    strictEqual(dec.decode(chunks[0]), 'hello');
    strictEqual(dec.decode(chunks[1]), 'there');

    ok(!ws.locked);
    ok(!readable.locked);

    const writer2 = ws.getWriter();
    await rejects(writer2.write('no'), {
      message: 'This WritableStream has been closed.',
    });
  },
};

// Test pipeTo error in internal readable aborts JS writable when preventAbort = false
export const pipeToInternalToJsError = {
  async test() {
    const enc = new TextEncoder();

    const { readable, writable } = new IdentityTransformStream();

    const ws = new WritableStream({
      write(chunk) {},
    });

    const pipe = readable.pipeTo(ws);

    writable.abort(new Error('boom'));

    await rejects(pipe, { message: 'boom' });

    const writer = ws.getWriter();
    await rejects(writer.write('hello'), { message: 'boom' });
  },
};

// Test pipeTo error in internal readable does not abort JS writable when preventAbort = true
export const pipeToInternalToJsErrorPrevent = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const ws = new WritableStream({
      write(chunk) {},
    });

    const pipe = readable.pipeTo(ws, { preventAbort: true });

    writable.abort(new Error('boom'));

    await rejects(pipe, { message: 'boom' });

    ok(!ws.locked);

    const writer = ws.getWriter();
    await writer.write('hello');
  },
};

// Test pipeTo closing internal readable closes JS writable when preventClose = false
export const pipeToInternalToJsClose = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const ws = new WritableStream({});

    const pipe = readable.pipeTo(ws);

    writable.close();

    await scheduler.wait(1);

    const writer = ws.getWriter();
    await rejects(writer.write('hello'), {
      message: 'This WritableStream has been closed.',
    });
  },
};

// Test pipeTo closing internal readable does not close JS writable when preventClose = true
export const pipeToInternalToJsClosePrevent = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const ws = new WritableStream({});

    const pipe = readable.pipeTo(ws, { preventClose: true });

    writable.close();

    await scheduler.wait(1);

    const writer = ws.getWriter();
    await writer.write('hello');
  },
};

// Test simple pipeTo JS-to-JS
export const pipeToJsToJsSimple = {
  async test() {
    const chunks = [1, 2, 3];
    const output = [];
    const readable = new ReadableStream({
      async pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const writable = new WritableStream({
      write(chunk) {
        output.push(chunk);
      },
    });

    await readable.pipeTo(writable);

    strictEqual(output[0], 1);
    strictEqual(output[1], 2);
    strictEqual(output[2], 3);
  },
};

// Test pipeTo error in JS readable aborts JS writable when preventAbort = false
export const pipeToJsToJsErrorReadable = {
  async test() {
    let abortCalled = false;
    const readable = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const writable = new WritableStream({
      abort(reason) {
        abortCalled = true;
      },
    });

    await rejects(readable.pipeTo(writable), { message: 'boom' });

    ok(abortCalled);
  },
};

// Test pipeTo error in JS readable does not abort JS writable when preventAbort = true
export const pipeToJsToJsErrorReadablePrevent = {
  async test() {
    let abortCalled = false;
    const readable = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const writable = new WritableStream({
      abort(reason) {
        abortCalled = true;
      },
    });

    const pipe = readable.pipeTo(writable, { preventAbort: true });

    await rejects(pipe, { message: 'boom' });

    ok(!abortCalled);
  },
};

// Test pipeTo error in JS writable cancels JS readable when preventCancel = false
export const pipeToJsToJsErrorWritable = {
  async test() {
    let cancelCalled = false;

    const readable = new ReadableStream({
      start(c) {
        c.enqueue('hello');
      },
      cancel() {
        cancelCalled = true;
      },
    });
    const writable = new WritableStream({
      write() {
        throw new Error('boom');
      },
    });

    const pipe = readable.pipeTo(writable);

    await rejects(pipe, { message: 'boom' });

    ok(cancelCalled);
  },
};

// Test pipeTo error in JS writable does not cancel JS readable when preventCancel = true
export const pipeToJsToJsErrorWritablePrevent = {
  async test() {
    const chunks = [1, 2];
    let cancelCalled = false;
    const readable = new ReadableStream({
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
      cancel() {
        cancelCalled = true;
      },
    });
    const writable = new WritableStream({
      write() {
        throw new Error('boom');
      },
    });

    const pipe = readable.pipeTo(writable, { preventCancel: true });

    await rejects(pipe, { message: 'boom' });

    ok(!cancelCalled);

    const reader = readable.getReader();
    await reader.read();
  },
};

// Test closing JS readable closes JS writable when preventClose = false
export const pipeToJsToJsCloseReadable = {
  async test() {
    let closeCalled = false;
    const readable = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const writable = new WritableStream({
      close() {
        closeCalled = true;
      },
    });

    await readable.pipeTo(writable);

    ok(closeCalled);
  },
};

// Test closing JS readable does not close JS writable when preventClose = true
export const pipeToJsToJsCloseReadablePrevent = {
  async test() {
    let closeCalled = false;
    const readable = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const writable = new WritableStream({
      close() {
        closeCalled = true;
      },
    });

    await readable.pipeTo(writable, { preventClose: true });

    ok(!closeCalled);
  },
};

// Test pipeTo from a tee branch
export const pipeToJsToJsTee = {
  async test() {
    const readable = new ReadableStream({
      start(c) {
        c.enqueue('hello');
        c.close();
      },
    });

    const output = [];
    const writable = new WritableStream({
      write(chunk) {
        output.push(chunk);
      },
    });

    const [branch] = readable.tee();

    await branch.pipeTo(writable);

    strictEqual(output[0], 'hello');
  },
};

// Test pipeTo with already-aborted signal (JS to JS)
export const pipeToJsToJsCancelAlready = {
  async test() {
    const signal = AbortSignal.abort(new Error('boom'));
    let writeCalled = false;
    const readable = new ReadableStream({
      start(c) {
        c.enqueue('hello');
        c.close();
      },
    });

    const writable = new WritableStream({
      write(chunk) {
        writeCalled = true;
      },
    });

    await rejects(readable.pipeTo(writable, { signal }), { message: 'boom' });

    ok(!writeCalled);
  },
};

// Test pipeTo with already-aborted signal (JS to native)
export const pipeToJsToNativeCancelAlready = {
  async test() {
    const signal = AbortSignal.abort(new Error('boom'));
    const source = new ReadableStream({
      start(c) {
        c.enqueue('hello');
        c.close();
      },
    });

    const { writable, readable } = new TransformStream();

    await rejects(source.pipeTo(writable, { signal }), { message: 'boom' });
  },
};

// Test pipeTo cancelable during operation (JS to JS)
export const pipeToJsToJsCancel = {
  async test() {
    const controller = new AbortController();

    const readable = new ReadableStream({
      start(c) {
        c.enqueue('hello');
      },
    });

    let output = '';
    const writable = new WritableStream({
      write(chunk) {
        output += chunk;
        controller.abort(new Error('boom'));
      },
    });

    await rejects(readable.pipeTo(writable, { signal: controller.signal }), {
      message: 'boom',
    });

    strictEqual(output, 'hello');
  },
};

// Test pipeTo cancelable during operation (JS to native)
export const pipeToJsToNativeCancel = {
  async test() {
    const controller = new AbortController();
    const enc = new TextEncoder();

    let ready = false;

    const source = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
      pull() {
        if (ready) {
          controller.abort(new Error('boom'));
        }
      },
    });

    const { readable, writable } = new TransformStream();

    const reader = readable.getReader();

    ready = true;

    const promises = await Promise.allSettled([
      source.pipeTo(writable, { signal: controller.signal }),
      reader.read(),
    ]);

    strictEqual(promises[0].status, 'rejected');
    strictEqual(promises[1].status, 'rejected');

    strictEqual(promises[0].reason.message, 'boom');
    strictEqual(promises[1].reason.message, 'boom');
  },
};

// Default fetch handler for service binding requests
export default {
  async fetch(request) {
    if (request.url.includes('/stream')) {
      const data = 'hello world '.repeat(100);
      return new Response(data);
    }
    return new Response('Not found', { status: 404 });
  },
};

// =============================================================================
// Porting status from {internal repo}/api-tests/streams/pipe.ew-test
//
// MOSTLY PORTED - Most tests from pipe.ew-test have been ported
//
// JS to Internal (pipeThrough/pipeTo):
// - simple-pipethrough-js-to-internal: Ported as pipeThroughJsToInternal
// - pipethrough-js-to-internal-errored-source: Ported as pipeThroughJsToInternalErroredSource
// - pipeto-js-to-internal-errored-source: Ported as pipeToJsToInternalErroredSource
// - pipethrough-js-to-internal-errored-source-prevent: Ported as pipeThroughJsToInternalErroredSourcePreventAbort
// - pipeto-js-to-internal-errored-source-prevent: Ported as pipeToJsToInternalErroredSourcePreventAbort
// - pipethrough-js-to-internal-errored-dest: Ported as pipeThroughJsToInternalErroredDest
// - pipeto-js-to-internal-errored-dest: Ported as pipeToJsToInternalErroredDest
// - pipethrough-js-to-internal-closes: Ported as pipeThroughJsToInternalCloses
// - pipethrough-js-to-internal-prevent-close: Ported as pipeThroughJsToInternalPreventClose
// - simple-pipethrough-js-byob-to-internal: Ported as pipeThroughJsByobToInternal
// - simple-pipeto-js-byob-to-internal: Ported as pipeToJsByobToInternal
//
// Internal to JS (pipeTo):
// - pipethrough-internal-to-js: Ported as pipeThroughInternalToJs
// - pipeto-internal-to-js: Ported as pipeToInternalToJs
// - simple-pipeto-internal-to-js: Ported as pipeToInternalToJsSimple
// - pipeto-internal-to-js-error: Ported as pipeToInternalToJsError
// - pipeto-internal-to-js-error-prevent: Ported as pipeToInternalToJsErrorPrevent
// - pipeto-internal-to-js-close: Ported as pipeToInternalToJsClose
// - pipeto-internal-to-js-close-prevent: Ported as pipeToInternalToJsClosePrevent
//
// JS to JS (pipeTo/pipeThrough):
// - pipethrough-js-to-js: Ported as pipeThroughJsToJs
// - simple-pipeto-js-to-js: Ported as pipeToJsToJsSimple
// - simple-pipeto-js-to-js-error-readable: Ported as pipeToJsToJsErrorReadable
// - simple-pipeto-js-to-js-error-readable-prevent: Ported as pipeToJsToJsErrorReadablePrevent
// - simple-pipeto-js-to-js-error-writable: Ported as pipeToJsToJsErrorWritable
// - simple-pipeto-js-to-js-error-writable-prevent: Ported as pipeToJsToJsErrorWritablePrevent
// - simple-pipeto-js-to-js-close-readable: Ported as pipeToJsToJsCloseReadable
// - simple-pipeto-js-to-js-close-readable-prevent: Ported as pipeToJsToJsCloseReadablePrevent
// - simple-pipeto-js-to-js-tee: Ported as pipeToJsToJsTee
//
// AbortSignal tests:
// - cancel-pipeto-js-to-js-already: Ported as pipeToJsToJsCancelAlready
// - cancel-pipeto-js-to-native-already: Ported as pipeToJsToNativeCancelAlready
// - cancel-pipeto-js-to-js: Ported as pipeToJsToJsCancel
// - cancel-pipeto-js-to-native: Ported as pipeToJsToNativeCancel
//
// NOT PORTED (require internal-only features or cause unhandled rejection errors):
// - simple-pipeto-js-to-internal: Causes unhandled promise rejection in wd-test
//   environment before the error can be caught (pipe rejects while consuming readable)
// - pipethrough-js-to-internal-errored-dest-prevent: Causes unhandled promise rejection
// - pipeto-js-to-internal-errored-dest-prevent: Causes unhandled promise rejection
// - never-ending-pipethrough: Tests CPU limit enforcement, requires internal harness
// - pipeto-double-close: Requires request body piping which needs HTTP request/response cycle
// - pipeto-internal-to-js-error-readable: Tests internal readable error propagation to JS writable,
//   requires more complex setup with IdentityTransformStream's internal behavior
// - pipeto-internal-to-js-error-readable-prevent: Same as above with preventCancel=true
// =============================================================================
