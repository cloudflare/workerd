// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The pipeTo/pipeThrough matrix, migrated wholesale from
// pipe-streams-test.js: JS-backed and native (IdentityTransformStream)
// endpoints in every direction, error/close propagation with the
// preventAbort/preventCancel/preventClose options, pre-aborted and
// mid-read AbortSignals, and the queued-destination close shapes.

import { strictEqual, ok, rejects, deepStrictEqual, throws } from 'node:assert';
import { mock } from 'node:test';
import { usingTsImpl } from 'which-impl';

// Writing to a closed JS WritableStream rejects with per-implementation
// messages.
const CLOSED_WRITE_MSG = usingTsImpl
  ? 'Cannot write to a stream that is closing or closed'
  : 'This WritableStream has been closed.';

// Test pipeThrough from JavaScript readable to internal writable
export const pipeThroughJsToInternal = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = [enc.encode('hello'), enc.encode('there'), 'hello', 123];
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });
    const transform = new IdentityTransformStream();
    const readable = rs.pipeThrough(transform);

    const output = [];
    if (usingTsImpl) {
      // DIVERGENCE: TypeScript ENCODES string chunks through the native
      // identity writable (UTF-8), and a NUMBER chunk stalls the pipe
      // silently — the next read pends forever (bounded observation;
      // defect shape). C++ rejects the pipe at the first non-byte chunk.
      const reader = readable.getReader();
      for (let i = 0; i < 3; i++) {
        output.push(dec.decode((await reader.read()).value));
      }
      deepStrictEqual(output, ['hello', 'there', 'hello']);
      const outcome = await Promise.race([
        reader.read().then(() => 'settled'),
        scheduler.wait(100).then(() => 'pending'),
      ]);
      strictEqual(outcome, 'pending');
      await reader.cancel('cleanup');
    } else {
      async function consumeStream() {
        for await (const chunk of readable) {
          output.push(dec.decode(chunk));
        }
      }
      // The 'hello' string at the end of chunks will cause an error to
      // be thrown when the pipe writes it to the byte-oriented identity.
      await rejects(consumeStream, {
        message: 'This WritableStream only supports writing byte types.',
      });
      deepStrictEqual(output, ['hello', 'there', 'hello']);
    }
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

// Test pipeThrough error in JS Readable does not abort Internal Writable when preventAbort = true
export const pipeThroughJsToInternalErroredSourcePreventAbort = {
  async test() {
    const enc = new TextEncoder();
    const _dec = new TextDecoder();
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

    // Allow the piping algorithm to process the error from the pull.
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

    // Allow the cancel to propagate back to the source.
    await scheduler.wait(1);

    ok(!rs.locked);
    ok(!transform.readable.locked);
    ok(!transform.writable.locked);

    // Our JavaScript ReadableStream should be closed (not errored).
    // Cancel propagates back and closes the source stream.
    const reader2 = rs.getReader();
    const result = await reader2.read();
    ok(result.done);
    strictEqual(result.value, undefined);
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

    // Our JavaScript ReadableStream should be closed (not errored).
    // Cancel propagates back and closes the source stream.
    const reader2 = rs.getReader();
    const result = await reader2.read();
    ok(result.done);
    strictEqual(result.value, undefined);
  },
};

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

    for await (const _chunk of readable) {
      // consume all chunks
    }

    // DIVERGENCE: after the pipe completes, C++ keeps the writable
    // locked (getWriter throws). Under TypeScript getWriter() SUCCEEDS;
    // the .locked getter's value at this point is transient (observed
    // both true and false across runs), so only the deterministic
    // getWriter behavior is pinned.
    if (usingTsImpl) {
      transform.writable.getWriter();
    } else {
      strictEqual(transform.writable.locked, true);
      throws(() => transform.writable.getWriter(), {
        message: 'This WritableStream is currently locked to a writer.',
      });
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
      message: CLOSED_WRITE_MSG,
    });
  },
};

// Test pipeTo error in internal readable aborts JS writable when preventAbort = false
export const pipeToInternalToJsError = {
  async test() {
    const _enc = new TextEncoder();

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

    readable.pipeTo(ws);

    writable.close();

    // Allow the close to propagate through the pipe.
    await scheduler.wait(1);

    const writer = ws.getWriter();
    await rejects(writer.write('hello'), {
      message: CLOSED_WRITE_MSG,
    });
  },
};

// Test pipeTo closing internal readable does not close JS writable when preventClose = true
export const pipeToInternalToJsClosePrevent = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const ws = new WritableStream({});

    readable.pipeTo(ws, { preventClose: true });

    writable.close();

    // Allow the pipe to finish without closing the destination.
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

    deepStrictEqual(output, [1, 2, 3]);
  },
};

// Test pipeTo error in JS readable aborts JS writable when preventAbort = false
export const pipeToJsToJsErrorReadable = {
  async test() {
    const abortFn = mock.fn();
    const readable = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const writable = new WritableStream({
      abort: abortFn,
    });

    await rejects(readable.pipeTo(writable), { message: 'boom' });

    strictEqual(abortFn.mock.callCount(), 1);
  },
};

// Test pipeTo error in JS readable does not abort JS writable when preventAbort = true
export const pipeToJsToJsErrorReadablePrevent = {
  async test() {
    const abortFn = mock.fn();
    const readable = new ReadableStream({
      async pull() {
        throw new Error('boom');
      },
    });
    const writable = new WritableStream({
      abort: abortFn,
    });

    const pipe = readable.pipeTo(writable, { preventAbort: true });

    await rejects(pipe, { message: 'boom' });

    strictEqual(abortFn.mock.callCount(), 0);
  },
};

// Test pipeTo error in JS writable cancels JS readable when preventCancel = false
export const pipeToJsToJsErrorWritable = {
  async test() {
    const cancelFn = mock.fn();
    const readable = new ReadableStream({
      start(c) {
        c.enqueue('hello');
      },
      cancel: cancelFn,
    });
    const writable = new WritableStream({
      write() {
        throw new Error('boom');
      },
    });

    const pipe = readable.pipeTo(writable);

    await rejects(pipe, { message: 'boom' });

    strictEqual(cancelFn.mock.callCount(), 1);
  },
};

// Test pipeTo error in JS writable does not cancel JS readable when preventCancel = true
export const pipeToJsToJsErrorWritablePrevent = {
  async test() {
    const chunks = [1, 2];
    const cancelFn = mock.fn();
    const readable = new ReadableStream({
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
      cancel: cancelFn,
    });
    const writable = new WritableStream({
      write() {
        throw new Error('boom');
      },
    });

    const pipe = readable.pipeTo(writable, { preventCancel: true });

    await rejects(pipe, { message: 'boom' });

    strictEqual(cancelFn.mock.callCount(), 0);

    const reader = readable.getReader();
    await reader.read();
  },
};

// Test closing JS readable closes JS writable when preventClose = false
export const pipeToJsToJsCloseReadable = {
  async test() {
    const closeFn = mock.fn();
    const readable = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const writable = new WritableStream({
      close: closeFn,
    });

    await readable.pipeTo(writable);

    strictEqual(closeFn.mock.callCount(), 1);
  },
};

// Test closing JS readable does not close JS writable when preventClose = true
export const pipeToJsToJsCloseReadablePrevent = {
  async test() {
    const closeFn = mock.fn();
    const readable = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const writable = new WritableStream({
      close: closeFn,
    });

    await readable.pipeTo(writable, { preventClose: true });

    strictEqual(closeFn.mock.callCount(), 0);
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
    const writeFn = mock.fn();
    const readable = new ReadableStream({
      start(c) {
        c.enqueue('hello');
        c.close();
      },
    });

    const writable = new WritableStream({
      write: writeFn,
    });

    await rejects(readable.pipeTo(writable, { signal }), { message: 'boom' });

    strictEqual(writeFn.mock.callCount(), 0);
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

    const { writable, readable: _readable } = new TransformStream();

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

// Test pipeTo aborted while a read is pending (JS source to internal writable).
//
// This exercises the internal pipe loop's abort path: the read continuation observes
// the aborted signal (Pipe::State::checkSignal -> Pipe::checkSignal), which cancels
// and releases the source (running this test's cancel algorithm synchronously) and
// then drains the destination queue — destroying the Pipe itself mid-call. Regression
// test for the pipe teardown hardening: in debug builds, holding a strong kj::Ptr to
// the Pipe across that call trips the PtrTarget liveness assert.
export const pipeToJsToInternalAbortMidRead = {
  async test() {
    const ac = new AbortController();
    const reason = new Error('boom');
    const cancelFn = mock.fn();

    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel: cancelFn,
    });

    const ts = new IdentityTransformStream();
    const pipePromise = rs.pipeTo(ts.writable, { signal: ac.signal });

    // Let the pipe loop start and issue a read (which remains pending since
    // nothing has been enqueued yet).
    await scheduler.wait(0);

    // Abort while the read is pending, then satisfy the read so the pipe loop's
    // read continuation runs and observes the aborted signal.
    ac.abort(reason);
    rc.enqueue(new TextEncoder().encode('hello'));

    await rejects(pipePromise, { message: 'boom' });

    // The source was canceled with the abort reason, exactly once (the release is
    // idempotent even though multiple cleanup paths run).
    strictEqual(cancelFn.mock.callCount(), 1);
    strictEqual(cancelFn.mock.calls[0].arguments[0], reason);

    // The source's pipe lock was released.
    strictEqual(rs.locked, false);

    // The destination was aborted: its readable side errors.
    await rejects(ts.readable.getReader().read(), { message: 'boom' });
  },
};

// Same as above with preventCancel: the source must not be canceled, but its pipe
// lock is still released (the pipe is over), so the stream remains usable.
export const pipeToJsToInternalAbortPreventCancel = {
  async test() {
    const ac = new AbortController();
    const reason = new Error('boom');
    const cancelFn = mock.fn();

    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel: cancelFn,
    });

    const ts = new IdentityTransformStream();
    const pipePromise = rs.pipeTo(ts.writable, {
      signal: ac.signal,
      preventCancel: true,
    });

    await scheduler.wait(0);
    ac.abort(reason);
    rc.enqueue(new TextEncoder().encode('hello'));

    await rejects(pipePromise, { message: 'boom' });

    strictEqual(cancelFn.mock.callCount(), 0);

    // The source lock was released even though the source was not canceled; the
    // stream must be lockable again.
    strictEqual(rs.locked, false);
    rs.getReader();
  },
};

// Same abort scenario with preventAbort: the destination is not aborted and remains
// usable after the pipe rejects. This exercises the non-drain teardown branch of the
// internal pipe loop's signal handling.
export const pipeToJsToInternalAbortPreventAbort = {
  async test() {
    const ac = new AbortController();
    const reason = new Error('boom');
    const cancelFn = mock.fn();

    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel: cancelFn,
    });

    const ts = new IdentityTransformStream();
    const pipePromise = rs.pipeTo(ts.writable, {
      signal: ac.signal,
      preventAbort: true,
    });

    await scheduler.wait(0);
    ac.abort(reason);
    rc.enqueue(new TextEncoder().encode('hello'));

    await rejects(pipePromise, { message: 'boom' });

    // The source was still canceled (preventCancel was not set).
    strictEqual(cancelFn.mock.callCount(), 1);
    strictEqual(rs.locked, false);

    // The destination was NOT aborted: it is unlocked and still writable. Prove it
    // by pushing a chunk through the identity transform.
    const writer = ts.writable.getWriter();
    const readerPromise = ts.readable.getReader().read();
    await writer.write(new TextEncoder().encode('after'));
    const { value, done } = await readerPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'after');
  },
};

// Test pipeTo aborted while a read is pending (JS source to JS writable).
//
// This exercises the standard pipe loop's abort path
// (WritableLockImpl::PipeLocked::checkSignal): the source is canceled and released
// (running this test's cancel algorithm synchronously while the write-side pipe lock
// still exists), the destination's abort algorithm runs with the reason, and the
// abort continuation settles the pipe promise after the write-side pipe lock has
// already been released — the continuation must not touch the destroyed lock state.
export const pipeToJsToJsAbortMidRead = {
  async test() {
    const ac = new AbortController();
    const reason = new Error('boom');
    const cancelFn = mock.fn();
    const abortFn = mock.fn();

    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel: cancelFn,
    });
    const ws = new WritableStream({ abort: abortFn });

    const pipePromise = rs.pipeTo(ws, { signal: ac.signal });

    await scheduler.wait(0);
    ac.abort(reason);
    rc.enqueue('hello');

    await rejects(pipePromise, { message: 'boom' });

    strictEqual(cancelFn.mock.callCount(), 1);
    strictEqual(cancelFn.mock.calls[0].arguments[0], reason);

    strictEqual(abortFn.mock.callCount(), 1);
    strictEqual(abortFn.mock.calls[0].arguments[0], reason);

    // Both locks were released.
    strictEqual(rs.locked, false);
    strictEqual(ws.locked, false);
  },
};

// Same as above with preventCancel: the source must not be canceled but is unlocked;
// the destination is still aborted.
export const pipeToJsToJsAbortPreventCancel = {
  async test() {
    const ac = new AbortController();
    const reason = new Error('boom');
    const cancelFn = mock.fn();
    const abortFn = mock.fn();

    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel: cancelFn,
    });
    const ws = new WritableStream({ abort: abortFn });

    const pipePromise = rs.pipeTo(ws, {
      signal: ac.signal,
      preventCancel: true,
    });

    await scheduler.wait(0);
    ac.abort(reason);
    rc.enqueue('hello');

    await rejects(pipePromise, { message: 'boom' });

    strictEqual(cancelFn.mock.callCount(), 0);
    strictEqual(abortFn.mock.callCount(), 1);
    strictEqual(abortFn.mock.calls[0].arguments[0], reason);

    strictEqual(rs.locked, false);
    rs.getReader();
  },
};

// Test pipeTo into a JS destination whose close was already queued when the pipe
// started (JS source to JS writable).
//
// The pipe loop only checks for a fully-Closed destination at the top of each
// iteration, so a close that is merely queued or in flight does not stop the pipe
// from starting and issuing a read. When the close algorithm then completes
// mid-pipe, WritableStreamJsController::doClose finds the write-side pipe lock
// still held. Regression test: doClose must release the source's pipe lock (and
// cancel the source, since closing propagates backward) rather than tearing down
// the write-side lock alone — which left the source permanently locked.
export const pipeToJsToJsCloseQueuedDestination = {
  async test() {
    const cancelFn = mock.fn();
    let resolveClose;
    const closePromise = new Promise((resolve) => (resolveClose = resolve));

    const rs = new ReadableStream({
      pull() {
        // Never enqueue anything; the pipe's first read stays pending.
      },
      cancel: cancelFn,
    });
    const ws = new WritableStream({
      close() {
        return closePromise;
      },
    });

    // Queue the close before the pipe starts. The stream is not locked
    // yet, so this is allowed; the close algorithm holds the close in
    // flight.
    const closed = ws.close();
    const pipePromise = rs.pipeTo(ws);

    if (usingTsImpl) {
      // DIVERGENCE: TypeScript rejects the pipe IMMEDIATELY on a
      // closing destination — no locks are ever observed held — and
      // cancels the source with the same TypeError.
      await rejects(pipePromise, {
        name: 'TypeError',
        message: 'Destination closed before the pipe completed',
      });
      strictEqual(rs.locked, false);
      strictEqual(ws.locked, false);
      resolveClose();
      await closed;
      strictEqual(cancelFn.mock.callCount(), 1);
      const reason = cancelFn.mock.calls[0].arguments[0];
      ok(reason instanceof TypeError);
      strictEqual(
        reason.message,
        'Destination closed before the pipe completed'
      );
      return;
    }

    // Let the pipe loop start and issue its read. The pipe holds both locks
    // while the close is still in flight.
    await scheduler.wait(0);
    strictEqual(rs.locked, true);
    strictEqual(ws.locked, true);

    // Complete the close mid-pipe.
    resolveClose();
    await closed;

    // The source was canceled with the backward-propagated close error and its
    // pipe lock was released.
    strictEqual(cancelFn.mock.callCount(), 1);
    const reason = cancelFn.mock.calls[0].arguments[0];
    ok(reason instanceof TypeError);
    strictEqual(reason.message, 'This destination writable stream is closed.');
    strictEqual(rs.locked, false);

    // The destination was unlocked too and remains usable as a closed stream.
    strictEqual(ws.locked, false);
    const writer = ws.getWriter();
    await writer.closed;

    // TODO(conform): The spec's "closing must be propagated backward" would have
    // rejected the pipe promise with the TypeError above. The pipe loop bails out
    // once the pipe lock is gone, so the promise currently resolves instead.
    await pipePromise;
  },
};

// Same as above with preventCancel: the source must not be canceled, but its pipe
// lock is still released (the pipe is over), so the stream remains usable.
export const pipeToJsToJsCloseQueuedDestinationPreventCancel = {
  async test() {
    const cancelFn = mock.fn();
    let resolveClose;
    const closePromise = new Promise((resolve) => (resolveClose = resolve));

    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel: cancelFn,
    });
    const ws = new WritableStream({
      close() {
        return closePromise;
      },
    });

    const closed = ws.close();
    const pipePromise = rs.pipeTo(ws, { preventCancel: true });

    if (usingTsImpl) {
      // DIVERGENCE: immediate rejection as in the non-prevent variant;
      // preventCancel additionally suppresses the source cancel.
      await rejects(pipePromise, {
        name: 'TypeError',
        message: 'Destination closed before the pipe completed',
      });
      strictEqual(cancelFn.mock.callCount(), 0);
      strictEqual(rs.locked, false);
      resolveClose();
      await closed;
      return;
    }

    await scheduler.wait(0);
    strictEqual(rs.locked, true);

    resolveClose();
    await closed;

    // The source lock was released even though the source was not canceled; the
    // stream must be lockable again.
    strictEqual(cancelFn.mock.callCount(), 0);
    strictEqual(rs.locked, false);
    const reader = rs.getReader();

    // Close the source to settle the pipe's still-pending read, which lets the
    // pipe promise settle.
    rc.close();
    await reader.closed;
    await pipePromise;
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
