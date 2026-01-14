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

// Ported from https://github.com/mafintosh/end-of-stream with
// permission from the author, Mathias Buus (@mafintosh).

/* eslint-disable @typescript-eslint/no-explicit-any, @typescript-eslint/no-unnecessary-condition, @typescript-eslint/no-unsafe-call, @typescript-eslint/no-unsafe-member-access */

import type { EventEmitter } from 'node:events'
import type Stream from 'node:stream'
import { addAbortListener } from 'node-internal:events'
import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_STREAM_PREMATURE_CLOSE,
} from 'node-internal:internal_errors'
import { once } from 'node-internal:internal_http_util'
import { nextTick } from 'node-internal:internal_process'
import type { Readable } from 'node-internal:streams_readable'
import type { Transform } from 'node-internal:streams_transform'

import {
  willEmitClose as _willEmitClose,
  isClosed,
  isNodeStream,
  isReadable,
  isReadableErrored,
  isReadableFinished,
  isReadableNodeStream,
  isReadableStream,
  isWritable,
  isWritableErrored,
  isWritableFinished,
  isWritableNodeStream,
  isWritableStream,
  kIsClosedPromise,
} from 'node-internal:streams_util'
import type { Writable } from 'node-internal:streams_writable'
import {
  validateAbortSignal,
  validateBoolean,
  validateFunction,
  validateObject,
} from 'node-internal:validators'

// eslint-disable-next-line @typescript-eslint/no-unnecessary-type-parameters
function isRequest<T extends EventEmitter>(stream: any): stream is T {
  return 'setHeader' in stream && typeof stream.abort === 'function'
}

export const nop = (): void => {}

type EOSOptions = {
  cleanup?: boolean
  error?: boolean
  readable?: boolean
  writable?: boolean
  signal?: AbortSignal
}

type Callback = (...args: unknown[]) => void

export function eos(
  stream: Readable | Writable | Transform,
  options: EOSOptions,
  callback: Callback,
): Callback
export function eos(options: EOSOptions, callback: Callback): Callback
export function eos(
  stream: Readable | Writable | Transform | EOSOptions,
  options: EOSOptions | Callback,
  callback?: Callback,
): Callback {
  if (arguments.length === 2) {
    // @ts-expect-error TS2322 Supports overloads
    callback = options
    options = {} as EOSOptions
  } else if (options == null) {
    options = {} as EOSOptions
  } else {
    validateObject(options, 'options')
  }
  validateFunction(callback, 'callback')
  validateAbortSignal((options as EOSOptions).signal, 'options.signal')

  // Avoid AsyncResource.bind() because it calls Object.defineProperties which
  // is a bottleneck here.
  callback = once(callback) as Callback

  if (isReadableStream(stream) || isWritableStream(stream)) {
    return eosWeb(stream, options as EOSOptions, callback)
  }

  if (!isNodeStream(stream)) {
    throw new ERR_INVALID_ARG_TYPE(
      'stream',
      ['ReadableStream', 'WritableStream', 'Stream'],
      stream,
    )
  }

  const readable =
    (options as EOSOptions).readable ?? isReadableNodeStream(stream)
  const writable =
    (options as EOSOptions).writable ?? isWritableNodeStream(stream)

  const wState = stream._writableState
  const rState = stream._readableState

  const onlegacyfinish = (): void => {
    if (!stream.writable) {
      onfinish()
    }
  }

  // TODO (ronag): Improve soft detection to include core modules and
  // common ecosystem modules that do properly emit 'close' but fail
  // this generic check.
  let willEmitClose =
    _willEmitClose(stream) &&
    isReadableNodeStream(stream) === readable &&
    isWritableNodeStream(stream) === writable

  let writableFinished = isWritableFinished(stream, false)
  const onfinish = (): void => {
    writableFinished = true
    // Stream should not be destroyed here. If it is that
    // means that user space is doing something differently and
    // we cannot trust willEmitClose.
    if (stream.destroyed) {
      willEmitClose = false
    }

    if (willEmitClose && (!stream.readable || readable)) {
      return
    }

    if (!readable || readableFinished) {
      callback?.call(stream)
    }
  }

  let readableFinished = isReadableFinished(stream, false)
  const onend = (): void => {
    readableFinished = true
    // Stream should not be destroyed here. If it is that
    // means that user space is doing something differently and
    // we cannot trust willEmitClose.
    if (stream.destroyed) {
      willEmitClose = false
    }

    if (willEmitClose && (!stream.writable || writable)) {
      return
    }

    if (!writable || writableFinished) {
      callback?.call(stream)
    }
  }

  const onerror = (err: Error): void => {
    callback?.call(stream, err)
  }

  let closed = isClosed(stream)

  const onclose = (): void => {
    closed = true

    const errored = isWritableErrored(stream) || isReadableErrored(stream)

    if (errored && typeof errored !== 'boolean') {
      return callback?.call(stream, errored)
    }

    if (readable && !readableFinished && isReadableNodeStream(stream, true)) {
      if (!isReadableFinished(stream, false))
        return callback?.call(stream, new ERR_STREAM_PREMATURE_CLOSE())
    }
    if (writable && !writableFinished) {
      if (!isWritableFinished(stream, false))
        return callback?.call(stream, new ERR_STREAM_PREMATURE_CLOSE())
    }

    callback?.call(stream)
  }

  const onclosed = (): void => {
    closed = true

    const errored = isWritableErrored(stream) || isReadableErrored(stream)

    if (errored && typeof errored !== 'boolean') {
      callback?.call(stream, errored)
      return
    }

    callback?.call(stream)
  }

  const onrequest = (): void => {
    stream.req?.on('finish', onfinish)
  }

  if (isRequest(stream)) {
    stream.on('complete', onfinish)
    if (!willEmitClose) {
      stream.on('abort', onclose)
    }
    if (stream.req) {
      onrequest()
    } else {
      stream.on('request', onrequest)
    }
  } else if (writable && !wState) {
    // legacy streams
    ;(stream as Stream).on('end', onlegacyfinish).on('close', onlegacyfinish)
  }

  // Not all streams will emit 'close' after 'aborted'.
  if (
    !willEmitClose &&
    'aborted' in stream &&
    typeof stream.aborted === 'boolean'
  ) {
    stream.on('aborted', onclose)
  }

  stream.on('end', onend)
  stream.on('finish', onfinish)
  if ((options as EOSOptions).error !== false) {
    stream.on('error', onerror)
  }
  stream.on('close', onclose)

  if (closed) {
    nextTick(onclose)
  } else if (wState?.errorEmitted || rState?.errorEmitted) {
    if (!willEmitClose) {
      nextTick(onclosed)
    }
  } else if (
    !readable &&
    (!willEmitClose || isReadable(stream)) &&
    (writableFinished || isWritable(stream) === false) &&
    (wState == null || wState.pendingcb === undefined || wState.pendingcb === 0)
  ) {
    nextTick(onclosed)
  } else if (
    !writable &&
    (!willEmitClose || isWritable(stream)) &&
    (readableFinished || isReadable(stream) === false)
  ) {
    nextTick(onclosed)
  } else if (rState && stream.req && stream.aborted) {
    nextTick(onclosed)
  }

  const cleanup = (): void => {
    callback = nop
    stream.removeListener('aborted', onclose)
    stream.removeListener('complete', onfinish)
    stream.removeListener('abort', onclose)
    stream.removeListener('request', onrequest)
    stream.req?.removeListener('finish', onfinish)
    stream.removeListener('end', onlegacyfinish)
    stream.removeListener('close', onlegacyfinish)
    stream.removeListener('finish', onfinish)
    stream.removeListener('end', onend)
    stream.removeListener('error', onerror)
    stream.removeListener('close', onclose)
  }

  if ((options as EOSOptions).signal && !closed) {
    const abort = (): void => {
      // Keep it because cleanup removes it.
      const endCallback = callback
      cleanup()
      endCallback?.call(
        stream,
        new AbortError(undefined, {
          cause: (options as EOSOptions).signal?.reason,
        }),
      )
    }
    if ((options as EOSOptions).signal?.aborted) {
      nextTick(abort)
    } else {
      const disposable = addAbortListener((options as EOSOptions).signal, abort)
      const originalCallback = callback
      callback = once((...args: unknown[]): void => {
        disposable[Symbol.dispose]()
        originalCallback.apply(stream, args)
      })
    }
  }

  return cleanup
}

function eosWeb(
  stream: ReadableStream | WritableStream,
  options: { signal?: AbortSignal },
  callback: (...args: unknown[]) => void,
): () => void {
  let isAborted = false
  let abort = nop
  if (options.signal) {
    abort = (): void => {
      isAborted = true
      callback.call(
        stream,
        new AbortError(undefined, { cause: options.signal?.reason }),
      )
    }
    if (options.signal.aborted) {
      nextTick(abort)
    } else {
      const disposable = addAbortListener(options.signal, abort)
      const originalCallback = callback
      callback = once((...args: unknown[]): void => {
        disposable[Symbol.dispose]()
        originalCallback.apply(stream, args)
      })
    }
  }
  const resolverFn = (...args: unknown[]): void => {
    if (!isAborted) {
      nextTick(() => {
        callback.apply(stream, args)
      })
    }
  }
  // @ts-expect-error TS7053 Symbols are not defined in types yet.
  stream[kIsClosedPromise].promise.then(resolverFn, resolverFn)
  return nop
}

export function finished(
  stream: Readable | Writable,
  opts: EOSOptions = {},
): Promise<void> {
  let autoCleanup = false
  if (opts.cleanup) {
    validateBoolean(opts.cleanup, 'cleanup')
    autoCleanup = opts.cleanup
  }
  return new Promise<void>((resolve, reject) => {
    const cleanup = eos(stream, opts, (err: unknown) => {
      if (autoCleanup) {
        cleanup()
      }
      if (err) {
        // eslint-disable-next-line @typescript-eslint/prefer-promise-reject-errors
        reject(err)
      } else {
        resolve()
      }
    })
  })
}

eos.finished = finished
