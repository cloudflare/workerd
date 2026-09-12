// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// stream.pipeline() and stream/promises pipeline() with web streams among
// the stages. A web ReadableStream (or a TransformStream's readable) is
// consumed through async iteration; a web WritableStream (or a
// TransformStream's writable) is fed through a writer that honors ready,
// closes at the end, and aborts on failure. The pump keeps the writer, so a
// web destination stays locked after the pipeline completes.

import { Readable, Writable, pipeline, promises } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

function bytesStream(...parts) {
  return new ReadableStream({
    start(controller) {
      for (const part of parts) controller.enqueue(enc.encode(part));
      controller.close();
    },
  });
}

// A web sink that records decoded chunks and lifecycle events.
function recordingSink() {
  const events = [];
  const stream = new WritableStream({
    write(chunk) {
      events.push(dec.decode(chunk));
    },
    close() {
      events.push('close');
    },
    abort(reason) {
      events.push(`abort:${reason?.message ?? reason}`);
    },
  });
  return { stream, events };
}

function recordingWritable() {
  const chunks = [];
  const writable = new Writable({
    write(chunk, encoding, callback) {
      chunks.push(chunk);
      callback();
    },
  });
  return { writable, chunks };
}

function run(...streams) {
  return new Promise((resolve) => pipeline(...streams, resolve));
}

// A web ReadableStream as the source of a node destination.
export const pipelineWebReadableToNodeWritable = {
  async test() {
    const { writable, chunks } = recordingWritable();
    strictEqual(await run(bytesStream('a', 'b'), writable), undefined);
    strictEqual(Buffer.concat(chunks).toString(), 'ab');
    strictEqual(writable.writableFinished, true);
  },
};

// A web WritableStream as the destination of a node source: chunks arrive
// in order, the sink is closed at the end, and the stream is left locked by
// the pump's writer.
export const pipelineNodeReadableToWebWritable = {
  async test() {
    const { stream, events } = recordingSink();
    const source = Readable.from([Buffer.from('a'), Buffer.from('b')], {
      objectMode: false,
    });
    strictEqual(await run(source, stream), undefined);
    deepStrictEqual(events, ['a', 'b', 'close']);
    strictEqual(stream.locked, true);
    strictEqual(source.destroyed, true);
  },
};

// A web TransformStream between node stages, and a fully web pipeline.
export const pipelineThroughWebTransform = {
  async test() {
    const upper = () =>
      new TransformStream({
        transform(chunk, controller) {
          controller.enqueue(enc.encode(dec.decode(chunk).toUpperCase()));
        },
      });
    const { writable, chunks } = recordingWritable();
    await run(
      Readable.from([Buffer.from('ab'), Buffer.from('cd')], {
        objectMode: false,
      }),
      upper(),
      writable
    );
    strictEqual(Buffer.concat(chunks).toString(), 'ABCD');

    const { stream, events } = recordingSink();
    await run(bytesStream('ef'), upper(), stream);
    deepStrictEqual(events, ['EF', 'close']);
  },
};

// A web TransformStream as the first stage is the pipeline's writable head:
// its readable is what flows on. (Nothing writes into it here, so closing
// its writable side lets the pipeline complete.)
export const pipelineWebTransformAsSource = {
  async test() {
    const transform = new TransformStream();
    const { writable, chunks } = recordingWritable();
    const done = run(transform, writable);
    const writer = transform.writable.getWriter();
    await writer.write(enc.encode('head'));
    await writer.close();
    strictEqual(await done, undefined);
    strictEqual(Buffer.concat(chunks).toString(), 'head');
  },
};

// An async generator stage between web streams.
export const pipelineGeneratorBetweenWebStreams = {
  async test() {
    const { stream, events } = recordingSink();
    await run(
      bytesStream('x', 'y'),
      async function* (source) {
        for await (const chunk of source) {
          yield enc.encode(`${dec.decode(chunk)}!`);
        }
      },
      stream
    );
    deepStrictEqual(events, ['x!', 'y!', 'close']);
  },
};

// A failing web sink fails the pipeline with the sink's error and destroys
// the node source. The pump's abort() of the sink is a no-op — the stream is
// already errored — so the sink's abort algorithm never runs.
export const pipelineWebSinkErrorFailsPipeline = {
  async test() {
    const boom = new Error('sink failed');
    const events = [];
    const sink = new WritableStream({
      write() {
        throw boom;
      },
      abort(reason) {
        events.push(reason);
      },
    });
    const source = new Readable({
      read() {
        this.push('data');
      },
    });
    strictEqual(await run(source, sink), boom);
    strictEqual(source.destroyed, true);
    await scheduler.wait(5);
    deepStrictEqual(events, []);
  },
};

// A failing web source fails the pipeline with its error and destroys the
// node destination.
export const pipelineWebSourceErrorFailsPipeline = {
  async test() {
    const boom = new Error('source failed');
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('partial'));
        controller.error(boom);
      },
    });
    const { writable } = recordingWritable();
    strictEqual(await run(source, writable), boom);
    strictEqual(writable.destroyed, true);
    strictEqual(writable.errored, boom);
  },
};

// Chunks pass through a web source untouched, promise-valued ones included:
// resolved, rejected and pending promises reach an objectMode node
// destination — and a web one — as the very objects that were enqueued, and
// the pipeline can still be aborted while the pending one is in flight.
export const pipelineWebSourcePreservesPromiseChunks = {
  async test() {
    const resolved = Promise.resolve('x');
    const rejected = Promise.reject(new Error('rejected chunk'));
    rejected.catch(() => {});
    const pending = new Promise(() => {});
    const promiseSource = () =>
      new ReadableStream({
        start(controller) {
          controller.enqueue(resolved);
          controller.enqueue(rejected);
          controller.enqueue(pending);
        },
      });

    const nodeChunks = [];
    const nodeSink = new Writable({
      objectMode: true,
      write(chunk, encoding, callback) {
        nodeChunks.push(chunk);
        callback();
      },
    });
    const source = promiseSource();
    const controller = new AbortController();
    const done = promises.pipeline(source, nodeSink, {
      signal: controller.signal,
    });
    // Bounded: a pump stuck on a promise chunk must fail here, not hang.
    for (let i = 0; i < 100 && nodeChunks.length < 3; i++) {
      await scheduler.wait(2);
    }
    // By identity: the deep comparator treats distinct promises as equal.
    strictEqual(nodeChunks.length, 3);
    strictEqual(nodeChunks[0], resolved);
    strictEqual(nodeChunks[1], rejected);
    strictEqual(nodeChunks[2], pending);
    controller.abort();
    await rejects(done, { name: 'AbortError' });
    strictEqual(source.locked, false);

    const webChunks = [];
    const webSink = new WritableStream({
      write(chunk) {
        webChunks.push(chunk);
      },
    });
    const webController = new AbortController();
    const webDone = promises.pipeline(promiseSource(), webSink, {
      signal: webController.signal,
    });
    for (let i = 0; i < 100 && webChunks.length < 3; i++) {
      await scheduler.wait(2);
    }
    strictEqual(webChunks.length, 3);
    strictEqual(webChunks[0], resolved);
    strictEqual(webChunks[1], rejected);
    strictEqual(webChunks[2], pending);
    webController.abort();
    await rejects(webDone, { name: 'AbortError' });
  },
};

// A web destination failing on its own while the source is idle — its
// controller erroring, or a write rejecting once the source has gone quiet —
// fails the pipeline at once with the sink's error and destroys the source.
export const pipelineWebSinkFailureWithIdleSource = {
  async test() {
    let sinkController;
    const erroring = new WritableStream({
      start(controller) {
        sinkController = controller;
      },
    });
    const idle = new Readable({ read() {} });
    const idleDone = run(idle, erroring);
    await scheduler.wait(5);
    const boom = new Error('sink errored while the source was idle');
    sinkController.error(boom);
    strictEqual(await idleDone, boom);
    strictEqual(idle.destroyed, true);

    const rejection = new Error('write rejected');
    const rejecting = new WritableStream({
      write() {
        return Promise.reject(rejection);
      },
    });
    let pushed = false;
    const single = new Readable({
      read() {
        if (!pushed) {
          pushed = true;
          this.push('only');
        }
      },
    });
    strictEqual(await run(single, rejecting), rejection);
    strictEqual(single.destroyed, true);
  },
};

// Aborting an idle, fully web-backed pipeline: the source's pending read is
// interrupted and the source cancelled, the destination is aborted, and the
// promise rejects with the AbortError.
export const promisesPipelineSignalAbortsIdleWebPipeline = {
  async test() {
    let cancelled;
    const source = new ReadableStream({
      pull() {
        return new Promise(() => {});
      },
      cancel(reason) {
        cancelled = reason;
      },
    });
    const aborts = [];
    const sink = new WritableStream({
      abort(reason) {
        aborts.push(reason);
      },
    });
    const controller = new AbortController();
    const done = promises.pipeline(source, sink, {
      signal: controller.signal,
    });
    await scheduler.wait(5);
    controller.abort();
    await rejects(done, { name: 'AbortError', code: 'ABORT_ERR' });
    strictEqual(cancelled?.name, 'AbortError');
    strictEqual(source.locked, false);
    strictEqual(aborts.length, 1);
    strictEqual(aborts[0].name, 'AbortError');
  },
};

// A node destination that fails while the web source is idle: the pump's
// pending read is interrupted, the source is cancelled with the failure,
// and the pipeline fails with it.
export const pipelineNodeSinkAsyncErrorInterruptsIdleWebSource = {
  async test() {
    const boom = new Error('late node sink failure');
    let cancelled;
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('a'));
      },
      cancel(reason) {
        cancelled = reason;
      },
    });
    const writable = new Writable({
      write(chunk, encoding, callback) {
        setTimeout(() => callback(boom), 5);
      },
    });
    strictEqual(await run(source, writable), boom);
    strictEqual(cancelled, boom);
    strictEqual(source.locked, false);
  },
};

// A failing node destination fails the pipeline and cancels the web source
// through the iterator's return.
export const pipelineNodeSinkErrorCancelsWebSource = {
  async test() {
    const boom = new Error('node sink failed');
    let cancelled = false;
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('a'));
      },
      cancel() {
        cancelled = true;
      },
    });
    const writable = new Writable({
      write(chunk, encoding, callback) {
        callback(boom);
      },
    });
    strictEqual(await run(source, writable), boom);
    await scheduler.wait(5);
    strictEqual(cancelled, true);
    strictEqual(source.locked, false);
  },
};

// stream/promises pipeline(): a trailing WritableStream is a destination,
// not the options bag.
export const promisesPipelineTrailingWebWritable = {
  async test() {
    const { stream, events } = recordingSink();
    await promises.pipeline(bytesStream('p', 'q'), stream);
    deepStrictEqual(events, ['p', 'q', 'close']);
  },
};

// stream/promises pipeline() with { end: false } leaves the web destination
// open (and still locked by the pump's writer).
export const promisesPipelineEndFalseLeavesWebWritableOpen = {
  async test() {
    const { stream, events } = recordingSink();
    await promises.pipeline(bytesStream('open'), stream, { end: false });
    deepStrictEqual(events, ['open']);
    strictEqual(stream.locked, true);
  },
};

// An aborted signal fails the pipeline with an AbortError: the node source
// is destroyed, the pump's iteration fails, and the web destination is
// aborted with an AbortError. (The pipeline completes once a pending sink
// write settles: the abort of the destination waits for it, as in Node.)
export const promisesPipelineSignalAbortsWebWritable = {
  async test() {
    const aborts = [];
    const sink = new WritableStream({
      write() {
        return scheduler.wait(2);
      },
      abort(reason) {
        aborts.push(reason);
      },
    });
    const source = new Readable({
      read() {
        this.push('stuck');
      },
    });
    const controller = new AbortController();
    const done = promises.pipeline(source, sink, {
      signal: controller.signal,
    });
    await scheduler.wait(5);
    controller.abort();
    await rejects(done, { name: 'AbortError', code: 'ABORT_ERR' });
    strictEqual(source.destroyed, true);
    await scheduler.wait(5);
    strictEqual(aborts.length, 1);
    strictEqual(aborts[0].name, 'AbortError');
  },
};
