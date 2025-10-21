// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* eslint-disable @typescript-eslint/no-explicit-any, @typescript-eslint/no-unnecessary-condition, @typescript-eslint/no-unsafe-call, @typescript-eslint/no-unsafe-member-access */

import type { EventEmitter } from 'node:events';

import { Readable } from 'node-internal:streams_readable';
import { Writable } from 'node-internal:streams_writable';
import { Transform } from 'node-internal:streams_transform';
import {
  validateObject,
  validateFunction,
  validateAbortSignal,
} from 'node-internal:validators';
import { once } from 'node-internal:internal_http_util';
import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_STREAM_PREMATURE_CLOSE,
} from 'node-internal:internal_errors';
import {
  isClosed,
  isReadable,
  isReadableNodeStream,
  isReadableFinished,
  isReadableErrored,
  isWritable,
  isWritableNodeStream,
  isWritableFinished,
  isWritableErrored,
  isNodeStream,
  willEmitClose,
  nop,
} from 'node-internal:streams_util';
import { nextTick } from 'node-internal:internal_process';

// TODO(later): We do not current implement Node.js' Request object. Might never?
// eslint-disable-next-line @typescript-eslint/no-unnecessary-type-parameters
export function isRequest<T extends EventEmitter>(
  stream: any
): stream is T & { abort: VoidFunction } {
  return (
    stream != null &&
    'setHeader' in stream &&
    typeof stream.abort === 'function'
  );
}

export type EOSOptions = {
  cleanup?: boolean;
  error?: boolean;
  readable?: boolean;
  writable?: boolean;
  signal?: AbortSignal;
};

type Callback = (...args: unknown[]) => void;

export function eos(
  stream: Readable | Writable | Transform,
  options: EOSOptions,
  callback: Callback
): Callback;
export function eos(options: EOSOptions, callback: Callback): Callback;
export function eos(
  stream: Readable | Writable | Transform | EOSOptions,
  options: EOSOptions | Callback,
  callback?: Callback
): Callback {
  let _options$readable, _options$writable;
  let opts: EOSOptions;
  if (arguments.length === 2) {
    callback = options as Callback;
    opts = {} as EOSOptions;
  } else if (options == null) {
    opts = {} as EOSOptions;
  } else {
    validateObject(options as EOSOptions, 'options');
    opts = options as EOSOptions;
  }
  validateFunction(callback, 'callback');
  validateAbortSignal(opts.signal, 'options.signal');
  callback = once(callback) as Callback;
  const readable =
    (_options$readable = opts.readable) !== null &&
    _options$readable !== undefined
      ? _options$readable
      : isReadableNodeStream(stream);
  const writable =
    (_options$writable = opts.writable) !== null &&
    _options$writable !== undefined
      ? _options$writable
      : isWritableNodeStream(stream);
  if (!isNodeStream(stream)) {
    // TODO: Webstreams.
    throw new ERR_INVALID_ARG_TYPE('stream', 'Stream', stream);
  }
  const wState = stream._writableState;
  const rState = stream._readableState;
  const onlegacyfinish = (): void => {
    if (!stream.writable) {
      onfinish();
    }
  };

  // TODO (ronag): Improve soft detection to include core modules and
  // common ecosystem modules that do properly emit 'close' but fail
  // this generic check.
  let _willEmitClose =
    willEmitClose(stream) &&
    isReadableNodeStream(stream) === readable &&
    isWritableNodeStream(stream) === writable;
  let writableFinished = isWritableFinished(stream, false);
  const onfinish = (): void => {
    writableFinished = true;
    // Stream should not be destroyed here. If it is that
    // means that user space is doing something differently and
    // we cannot trust willEmitClose.
    if (stream.destroyed) {
      _willEmitClose = false;
    }
    if (_willEmitClose && (!stream.readable || readable)) {
      return;
    }
    if (!readable || readableFinished) {
      callback?.call(stream);
    }
  };
  let readableFinished = isReadableFinished(stream as Readable, false);
  const onend = (): void => {
    readableFinished = true;
    // Stream should not be destroyed here. If it is that
    // means that user space is doing something differently and
    // we cannot trust willEmitClose.
    if (stream.destroyed) {
      _willEmitClose = false;
    }
    if (_willEmitClose && (!stream.writable || writable)) {
      return;
    }
    if (!writable || writableFinished) {
      callback?.call(stream);
    }
  };
  const onerror = (err: any): void => {
    callback?.call(stream, err);
  };
  let closed = isClosed(stream);
  const onclose = (): void => {
    closed = true;
    const errored = isWritableErrored(stream) || isReadableErrored(stream);
    if (errored && typeof errored !== 'boolean') {
      return callback?.call(stream, errored);
    }
    if (readable && !readableFinished && isReadableNodeStream(stream, true)) {
      if (!isReadableFinished(stream, false))
        return callback?.call(stream, new ERR_STREAM_PREMATURE_CLOSE());
    }
    if (writable && !writableFinished) {
      if (!isWritableFinished(stream, false))
        return callback?.call(stream, new ERR_STREAM_PREMATURE_CLOSE());
    }
    callback?.call(stream);
  };
  const onrequest = (): void => {
    stream.req.on('finish', onfinish);
  };
  if (isRequest(stream)) {
    stream.on('complete', onfinish);
    if (!_willEmitClose) {
      stream.on('abort', onclose);
    }
    if (stream.req) {
      onrequest();
    } else {
      stream.on('request', onrequest);
    }
  } else if (writable && !wState) {
    // legacy streams
    stream.on('end', onlegacyfinish);
    stream.on('close', onlegacyfinish);
  }

  // Not all streams will emit 'close' after 'aborted'.
  if (!_willEmitClose && typeof stream.aborted === 'boolean') {
    stream.on('aborted', onclose);
  }
  stream.on('end', onend);
  stream.on('finish', onfinish);
  if (opts.error !== false) {
    stream.on('error', onerror);
  }
  stream.on('close', onclose);
  if (closed) {
    nextTick(onclose);
  } else if (
    (wState !== null && wState !== undefined && wState.errorEmitted) ||
    (rState !== null && rState !== undefined && rState.errorEmitted)
  ) {
    if (!_willEmitClose) {
      nextTick(onclose);
    }
  } else if (
    !readable &&
    (!_willEmitClose || isReadable(stream)) &&
    (writableFinished || isWritable(stream) === false)
  ) {
    nextTick(onclose);
  } else if (
    !writable &&
    (!_willEmitClose || isWritable(stream)) &&
    (readableFinished || isReadable(stream) === false)
  ) {
    nextTick(onclose);
  } else if (rState && stream.req && stream.aborted) {
    nextTick(onclose);
  }
  const cleanup = (): void => {
    callback = nop;
    stream.removeListener('aborted', onclose);
    stream.removeListener('complete', onfinish);
    stream.removeListener('abort', onclose);
    stream.removeListener('request', onrequest);
    if (stream.req) stream.req.removeListener('finish', onfinish);
    stream.removeListener('end', onlegacyfinish);
    stream.removeListener('close', onlegacyfinish);
    stream.removeListener('finish', onfinish);
    stream.removeListener('end', onend);
    stream.removeListener('error', onerror);
    stream.removeListener('close', onclose);
  };
  if (opts.signal && !closed) {
    const abort = (): void => {
      // Keep it because cleanup removes it.
      const endCallback = callback;
      cleanup();
      endCallback?.call(
        stream,
        new AbortError(undefined, {
          cause: opts.signal?.reason,
        })
      );
    };
    if (opts.signal.aborted) {
      nextTick(abort);
    } else {
      const originalCallback = callback;
      callback = once((...args) => {
        opts.signal?.removeEventListener('abort', abort);
        originalCallback.apply(stream, args);
      });
      opts.signal.addEventListener('abort', abort);
    }
  }
  return cleanup;
}

export function finished(
  stream: Readable | Writable,
  opts: EOSOptions = {}
): Promise<void> {
  return new Promise((resolve, reject) => {
    eos(stream, opts, (err) => {
      if (err) {
        reject(err as Error);
      } else {
        resolve();
      }
    });
  });
}

eos.finished = finished;
