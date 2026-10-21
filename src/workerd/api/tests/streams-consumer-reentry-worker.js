// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { inspect } from 'node:util';

let stream, controller, reader, sibling, byobRequest, writer;
const ignore = (promise) => promise.catch(() => {});

let partial;
async function partialResponse(url) {
  if (url.pathname === '/origin') {
    const state = {};
    partial = state;
    const source = new ReadableStream({
      type: 'bytes',
      start(value) {
        state.controller = value;
      },
      pull() {
        if (!state.finishOnPull) return;
        state.finishOnPull = false;
        const request = state.controller.byobRequest;
        state.progress = {
          offset: request.view.byteOffset,
          length: request.view.byteLength,
        };
        request.view[0] = 34;
        request.respond(1);
      },
    });
    const [first, rest] = source.tee();
    const [second, notification] = rest.tee();
    state.first = first.getReader({ mode: 'byob' });
    state.second = second.getReader({ mode: 'byob' });
    // This default read supplies only the expired-request warning callback. It
    // creates no BYOB request; the two observable reads start in the next request.
    state.notification = notification.getReader();
    ignore(state.notification.closed);
    ignore(state.notification.read());
    return new Response('origin complete');
  }

  const state = partial;
  const first = state.first.read(new Uint8Array(8), { min: 2 });
  const second = state.second.read(new Uint8Array(8), { min: 2 });
  // Let pending pull completions drain so the response's follow-up pull is synchronous.
  await scheduler.wait(0);
  const request = state.controller.byobRequest;
  request.view[0] = 17;
  let entries = 0;
  let nestedError = null;
  let nestedViewLength;
  const descriptor = Object.getOwnPropertyDescriptor(Error.prototype, 'name');
  Object.defineProperty(Error.prototype, 'name', {
    configurable: true,
    get() {
      return 'Error';
    },
    set() {
      if (entries !== 0) return;
      entries++;
      const view = request.view.subarray(0, 1);
      try {
        if (url.searchParams.get('method') === 'new-view') {
          request.respondWithNewView(view);
        } else {
          request.respond(1);
        }
      } catch (error) {
        nestedError = error.name;
      }
      nestedViewLength = view.byteLength;
    },
  });
  state.finishOnPull = url.searchParams.get('completion') === 'pull';
  try {
    request.respond(1);
  } finally {
    Object.defineProperty(Error.prototype, 'name', descriptor);
  }
  if (url.searchParams.get('completion') === 'pull') {
    if (!state.progress)
      throw new Error('The follow-up pull did not run synchronously');
  } else {
    state.progress = {
      offset: request.view.byteOffset,
      length: request.view.byteLength,
    };
    request.view[0] = 34;
    request.respond(1);
  }
  const results = await Promise.all([first, second]);
  state.controller.close();
  const closed = await Promise.all([
    state.first.read(new Uint8Array(8)),
    state.second.read(new Uint8Array(8)),
  ]);
  return Response.json({
    entries,
    nestedError,
    nestedViewLength,
    progress: state.progress,
    values: results.map(({ value }) => Array.from(value)),
    done: results.map(({ done }) => done),
    closed: closed.map(({ done }) => done),
  });
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const operation = url.searchParams.get('operation');
    if (operation === 'partial-respond') return partialResponse(url);
    const kind = url.searchParams.get('kind');
    const action = url.searchParams.get('action');
    const read = () =>
      kind === 'byob' ? reader.read(new Uint8Array(8)) : reader.read();
    if (url.pathname === '/origin') {
      if (operation === 'writer') {
        stream = new WritableStream({
          write() {
            return new Promise(() => {});
          },
        });
        writer = stream.getWriter();
        ignore(writer.closed);
        ignore(writer.ready);
        for (let i = 0; i < 12; i++) ignore(writer.write(i));
      } else {
        const source = {
          start(value) {
            controller = value;
          },
        };
        if (kind !== 'values') source.type = 'bytes';
        stream = new ReadableStream(source);
        if (operation === 'respond') {
          const branches = stream.tee();
          reader = branches[0].getReader({ mode: 'byob' });
          sibling = branches[1].getReader({ mode: 'byob' });
          ignore(reader.closed);
          ignore(sibling.closed);
          ignore(reader.read(new Uint8Array(8)));
          byobRequest = controller.byobRequest;
          ignore(sibling.read(new Uint8Array(8)));
        } else {
          reader = stream.getReader(
            kind === 'byob' ? { mode: 'byob' } : undefined
          );
          ignore(reader.closed);
          for (let i = 0; i < 12; i++) ignore(read());
          byobRequest = kind === 'byob' ? controller.byobRequest : undefined;
        }
      }
      return new Response('origin complete');
    }

    const current =
      operation === 'cancel'
        ? read().then(
            (result) => ({ status: 'fulfilled', done: result.done }),
            (error) => ({ status: 'rejected', error: String(error) })
          )
        : undefined;
    let armed = true;
    let entries = 0;
    let pipe, replacement, replacementWrite;
    const values = [];
    const nestedErrors = [];
    let outerError = null;
    const destination = () =>
      new WritableStream({
        write(value) {
          values.push(kind === 'values' ? value : Array.from(value));
        },
      });
    const descriptor = Object.getOwnPropertyDescriptor(Error.prototype, 'name');
    // Settling a read from a completed request emits a warning. Constructing that
    // warning synchronously invokes this setter, before settlement returns to C++.
    Object.defineProperty(Error.prototype, 'name', {
      configurable: true,
      get() {
        return 'Error';
      },
      set() {
        if (!armed) return;
        armed = false;
        entries++;
        if (action === 'retain') return;
        try {
          if (operation === 'writer') {
            writer.releaseLock();
            if (action === 'replace') {
              replacement = stream.getWriter();
              ignore(replacement.closed);
              ignore(replacement.ready);
              replacementWrite = replacement.write('replacement');
              ignore(replacementWrite);
            }
          } else if (action === 'respond') {
            byobRequest.respond(1);
          } else if (action === 'cancel') {
            ignore(reader.cancel('synthetic cancellation'));
          } else if (action === 'close') {
            controller.close();
          } else if (action === 'error') {
            controller.error(new Error('synthetic source error'));
          } else {
            reader.releaseLock();
            if (operation === 'release') {
              pipe = stream.pipeTo(destination());
              ignore(pipe);
            }
          }
        } catch (error) {
          nestedErrors.push(String(error));
        }
      },
    });
    try {
      if (operation === 'writer') writer.releaseLock();
      else if (operation === 'release') reader.releaseLock();
      else if (operation === 'cancel') await reader.cancel();
      else if (url.searchParams.get('method') === 'new-view') {
        byobRequest.respondWithNewView(byobRequest.view.subarray(0, 1));
      } else byobRequest.respond(1);
    } catch (error) {
      outerError = String(error);
    } finally {
      armed = false;
      Object.defineProperty(Error.prototype, 'name', descriptor);
    }

    const result = { entries, nestedErrors, outerError };
    if (operation === 'release') {
      if (!pipe) pipe = stream.pipeTo(destination());
      result.lockedDuringPipe = stream.locked;
      controller.enqueue(
        kind === 'values' ? 'healthy' : new Uint8Array([1, 2, 3])
      );
      controller.close();
      await pipe;
      result.lockedAfterPipe = stream.locked;
      result.values = values;
    } else if (operation === 'cancel') {
      result.current = await current;
    } else if (operation === 'writer') {
      result.locked = stream.locked;
      result.beforeCleanup = replacementWrite
        ? inspect(replacementWrite)
        : null;
      if (replacement) {
        replacement.releaseLock();
        try {
          await replacementWrite;
          result.afterCleanup = 'fulfilled';
        } catch (error) {
          result.afterCleanup =
            error instanceof TypeError ? 'rejected' : 'wrong error';
        }
      }
    }
    return Response.json(result);
  },
};
