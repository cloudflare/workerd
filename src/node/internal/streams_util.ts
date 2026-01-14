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

/* eslint-disable @typescript-eslint/no-explicit-any, @typescript-eslint/no-unsafe-member-access, @typescript-eslint/no-unnecessary-condition */

import { AbortError } from 'node-internal:internal_errors'
import type { IncomingMessage } from 'node-internal:internal_http_incoming'
import type { OutgoingMessage } from 'node-internal:internal_http_outgoing'
import type { ServerResponse } from 'node-internal:internal_http_server'
import { constants } from 'node-internal:internal_zlib_constants'
import type { Readable } from 'node-internal:streams_readable'
import type { Transform } from 'node-internal:streams_transform'
import type { Writable } from 'node-internal:streams_writable'

// We need to use Symbol.for to make these globally available
// for interoperability with readable-stream, i.e. readable-stream
// and node core needs to be able to read/write private state
// from each other for proper interoperability.
export const kIsDestroyed = Symbol.for('nodejs.stream.destroyed')
export const kIsErrored = Symbol.for('nodejs.stream.errored')
export const kIsReadable = Symbol.for('nodejs.stream.readable')
export const kIsWritable = Symbol.for('nodejs.stream.writable')
export const kIsDisturbed = Symbol.for('nodejs.stream.disturbed')

export const kOnConstructed = Symbol('kOnConstructed')

export const kIsClosedPromise = Symbol.for('nodejs.webstream.isClosedPromise')
export const kControllerErrorFunction = Symbol.for(
  'nodejs.webstream.controllerErrorFunction',
)

export const kState = Symbol('kState')
export const kObjectMode = 1 << 0
export const kErrorEmitted = 1 << 1
export const kAutoDestroy = 1 << 2
export const kEmitClose = 1 << 3
export const kDestroyed = 1 << 4
export const kClosed = 1 << 5
export const kCloseEmitted = 1 << 6
export const kErrored = 1 << 7
export const kConstructed = 1 << 8

export function isReadableNodeStream(
  obj: any,
  strict: boolean = false,
): boolean {
  return !!(
    (
      obj &&
      typeof obj.pipe === 'function' &&
      typeof obj.on === 'function' &&
      (!strict ||
        (typeof obj.pause === 'function' &&
          typeof obj.resume === 'function')) &&
      (!obj._writableState || obj._readableState?.readable !== false) && // Duplex
      (!obj._writableState || obj._readableState)
    ) // Writable has .pipe.
  )
}

export function isWritableNodeStream(obj: any): boolean {
  return !!(
    (
      obj &&
      typeof obj.write === 'function' &&
      typeof obj.on === 'function' &&
      (!obj._readableState || obj._writableState?.writable !== false)
    ) // Duplex
  )
}

export function isDuplexNodeStream(obj: any): boolean {
  return !!(
    obj &&
    typeof obj.pipe === 'function' &&
    obj._readableState &&
    typeof obj.on === 'function' &&
    typeof obj.write === 'function'
  )
}

export function isNodeStream(obj: any): obj is Readable | Writable | Transform {
  // eslint-disable-next-line @typescript-eslint/no-unsafe-return
  return (
    obj &&
    (obj._readableState != null ||
      obj._writableState != null ||
      (typeof obj.write === 'function' && typeof obj.on === 'function') ||
      (typeof obj.pipe === 'function' && typeof obj.on === 'function'))
  )
}

export function isReadableStream(obj: any): obj is ReadableStream {
  return !!(
    obj &&
    !isNodeStream(obj) &&
    typeof obj.pipeThrough === 'function' &&
    typeof obj.getReader === 'function' &&
    typeof obj.cancel === 'function'
  )
}

export function isWritableStream(obj: any): obj is WritableStream {
  return !!(
    obj &&
    !isNodeStream(obj) &&
    typeof obj.getWriter === 'function' &&
    typeof obj.abort === 'function'
  )
}

export function isTransformStream(obj: any): obj is TransformStream {
  return !!(
    obj &&
    !isNodeStream(obj) &&
    typeof obj.readable === 'object' &&
    typeof obj.writable === 'object'
  )
}

export function isWebStream(
  obj: any,
): obj is ReadableStream | WritableStream | TransformStream {
  return (
    isReadableStream(obj) || isWritableStream(obj) || isTransformStream(obj)
  )
}

export function isIterable(obj: any, isAsync?: true | false): boolean {
  if (obj == null) return false
  if (isAsync === true) return typeof obj[Symbol.asyncIterator] === 'function'
  if (isAsync === false) return typeof obj[Symbol.iterator] === 'function'
  return (
    typeof obj[Symbol.asyncIterator] === 'function' ||
    typeof obj[Symbol.iterator] === 'function'
  )
}

export function isDestroyed(stream: any): boolean | null {
  if (!isNodeStream(stream)) return null
  const wState = stream._writableState
  const rState = stream._readableState
  const state = wState || rState
  return !!(stream.destroyed || stream[kIsDestroyed] || state?.destroyed)
}

// Have been end():d.
export function isWritableEnded(
  stream: Writable | Readable | Transform,
): boolean | null {
  if (!isWritableNodeStream(stream)) return null
  if (stream.writableEnded === true) return true
  const wState = stream._writableState
  if (wState?.errored) return false
  if (typeof wState?.ended !== 'boolean') return null
  return wState.ended
}

// Have emitted 'finish'.
export function isWritableFinished(
  stream: Writable | Readable | Transform,
  strict?: true | false | null,
): boolean | null {
  if (!isWritableNodeStream(stream)) return null
  if (stream.writableFinished === true) return true
  const wState = stream._writableState
  if (wState?.errored) return false
  if (typeof wState?.finished !== 'boolean') return null
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-type-conversion
  return !!(
    wState.finished ||
    (strict === false && wState.ended === true && wState.length === 0)
  )
}

// Have been push(null):d.
export function isReadableEnded(
  stream: Readable | Writable | Transform,
): boolean | null {
  if (!isReadableNodeStream(stream)) return null
  if (stream.readableEnded === true) return true
  const rState = stream._readableState
  if (!rState || rState.errored) return false
  if (typeof rState?.ended !== 'boolean') return null
  return rState.ended
}

// Have emitted 'end'.
export function isReadableFinished(
  stream: Readable | Writable | Transform,
  strict?: boolean,
): boolean | null {
  if (!isReadableNodeStream(stream)) return null
  const rState = stream._readableState
  if (rState?.errored) return false
  if (typeof rState?.endEmitted !== 'boolean') return null
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-type-conversion
  return !!(
    rState.endEmitted ||
    (strict === false && rState.ended === true && rState.length === 0)
  )
}

export function isReadable(
  stream: Readable | Writable | Transform,
): boolean | null {
  if (stream && stream[kIsReadable] != null) return stream[kIsReadable]
  if (typeof stream?.readable !== 'boolean') return null
  if (isDestroyed(stream)) return false
  return (
    isReadableNodeStream(stream) &&
    stream.readable &&
    !isReadableFinished(stream)
  )
}

export function isWritable(
  stream: Readable | Writable | Transform,
): boolean | null {
  if (stream && stream[kIsWritable] != null) return stream[kIsWritable]
  if (typeof stream?.writable !== 'boolean') return null
  if (isDestroyed(stream)) return false
  return (
    isWritableNodeStream(stream) && stream.writable && !isWritableEnded(stream)
  )
}

export function isFinished(
  stream: Readable | Writable | Transform,
  opts?: { readable?: boolean; writable?: boolean },
): boolean | null {
  if (!isNodeStream(stream)) {
    return null
  }

  if (isDestroyed(stream)) {
    return true
  }

  if (opts?.readable !== false && isReadable(stream)) {
    return false
  }

  if (opts?.writable !== false && isWritable(stream)) {
    return false
  }

  return true
}

export function isWritableErrored(
  stream: Writable | Readable | Transform,
): Error | boolean | null {
  if (!isNodeStream(stream)) {
    return null
  }

  if (stream.writableErrored) {
    return stream.writableErrored
  }

  return stream._writableState?.errored ?? null
}

export function isReadableErrored(
  stream: Readable | Writable | Transform,
): Error | boolean | null {
  if (!isNodeStream(stream)) {
    return null
  }

  if (stream.readableErrored) {
    return stream.readableErrored
  }

  return stream._readableState?.errored ?? null
}

export function isClosed(
  stream: Readable | Writable | Transform,
): boolean | null {
  if (!isNodeStream(stream)) {
    return null
  }

  if (typeof stream.closed === 'boolean') {
    return stream.closed
  }

  const wState = stream._writableState
  const rState = stream._readableState

  if (
    typeof wState?.closed === 'boolean' ||
    typeof rState?.closed === 'boolean'
  ) {
    return (wState?.closed || rState?.closed) ?? null
  }

  if (typeof stream._closed === 'boolean' && isOutgoingMessage(stream)) {
    return stream._closed
  }

  return null
}

export function isOutgoingMessage(stream: unknown): stream is OutgoingMessage {
  return (
    stream != null &&
    typeof stream === 'object' &&
    '_closed' in stream &&
    typeof stream._closed === 'boolean' &&
    '_defaultKeepAlive' in stream &&
    typeof stream._defaultKeepAlive === 'boolean' &&
    '_removedConnection' in stream &&
    typeof stream._removedConnection === 'boolean' &&
    '_removedContLen' in stream &&
    typeof stream._removedContLen === 'boolean'
  )
}

// This function includes the following check that we don't include, because
// our ServerResponse implementation does not implement it.
// `typeof stream._sent100 === 'boolean'`
export function isServerResponse(stream: unknown): stream is ServerResponse {
  return isOutgoingMessage(stream)
}

export function isServerRequest(stream: any): stream is IncomingMessage {
  return (
    typeof stream._consuming === 'boolean' &&
    typeof stream._dumped === 'boolean' &&
    stream.req?.upgradeOrConnect === undefined
  )
}

export function willEmitClose(stream: any): boolean | null {
  if (!isNodeStream(stream)) return null

  const wState = stream._writableState
  const rState = stream._readableState
  const state = wState || rState

  return (
    (!state && isServerResponse(stream)) ||
    !!(state?.autoDestroy && state.emitClose && state.closed === false)
  )
}

export function isDisturbed(stream: any): boolean {
  return !!(
    stream &&
    (stream[kIsDisturbed] ?? (stream.readableDidRead || stream.readableAborted))
  )
}

export function isErrored(stream: any): boolean {
  return !!(
    stream &&
    (stream[kIsErrored] ??
      stream.readableErrored ??
      stream.writableErrored ??
      stream._readableState?.errorEmitted ??
      stream._writableState?.errorEmitted ??
      stream._readableState?.errored ??
      stream._writableState?.errored)
  )
}

const ZLIB_FAILURES = new Set<string>([
  ...Object.entries(constants).flatMap(([code, value]) =>
    value < 0 ? code : [],
  ),
  'Z_NEED_DICT',
])

export function handleKnownInternalErrors(
  cause?: Error & { code?: string },
): (Error & { code?: string }) | undefined {
  switch (true) {
    case cause?.code === 'ERR_STREAM_PREMATURE_CLOSE': {
      return new AbortError(undefined, { cause })
    }
    case ZLIB_FAILURES.has(cause?.code ?? ''): {
      const error = new TypeError(undefined, { cause }) as Error & {
        code?: string
      }
      error.code = cause?.code as string
      return error
    }
    default:
      return cause
  }
}
