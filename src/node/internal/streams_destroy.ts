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

/* eslint-disable @typescript-eslint/no-redundant-type-constituents, @typescript-eslint/no-unsafe-call */

import {
  AbortError,
  aggregateTwoErrors,
  ERR_MULTIPLE_CALLBACK,
} from 'node-internal:internal_errors'
import type { OutgoingMessage } from 'node-internal:internal_http_outgoing'
import { nextTick } from 'node-internal:internal_process'
import type { Readable, ReadableState } from 'node-internal:streams_readable'
import {
  isDestroyed,
  isFinished,
  isServerRequest,
  kAutoDestroy,
  kClosed,
  kCloseEmitted,
  kConstructed,
  kDestroyed,
  kEmitClose,
  kErrorEmitted,
  kErrored,
  kIsDestroyed,
  kState,
} from 'node-internal:streams_util'
import type { Writable, WritableState } from 'node-internal:streams_writable'

const kConstruct = Symbol('kConstruct')
const kDestroy = Symbol('kDestroy')

function checkError(
  err?: Error | null,
  w?: WritableState,
  r?: ReadableState,
): void {
  if (err) {
    // Avoid V8 leak, https://github.com/nodejs/node/pull/34103#issuecomment-652002364
    err.stack // eslint-disable-line @typescript-eslint/no-unused-expressions

    if (w && !w.errored) {
      w.errored = err
    }
    if (r && !r.errored) {
      r.errored = err
    }
  }
}

// Backwards compat. cb() is undocumented and unused in core but
// unfortunately might be used by modules.
export function destroy(
  this: Readable | Writable,
  err?: Error,
  cb?: VoidFunction,
  // @ts-expect-error TS2526 Returning this is not allowed.
): this {
  const r = this._readableState
  const w = this._writableState
  // With duplex streams we use the writable side for state.
  const s = w || r
  if (
    (w && (w[kState] & kDestroyed) !== 0) ||
    (r && (r[kState] & kDestroyed) !== 0)
  ) {
    if (typeof cb === 'function') {
      cb()
    }
    return this
  }

  // We set destroyed to true before firing error callbacks in order
  // to make it re-entrance safe in case destroy() is called within callbacks
  checkError(err, w, r)
  if (w) {
    w[kState] |= kDestroyed
  }
  if (r) {
    r[kState] |= kDestroyed
  }

  // If still constructing then defer calling _destroy.
  // @ts-expect-error TS18048 `s` will always be defined here.
  if ((s[kState] & kConstructed) === 0) {
    this.once(kDestroy, function (this: Readable | Writable, er: Error) {
      _destroy(this, aggregateTwoErrors(er, err), cb)
    })
  } else {
    _destroy(this, err, cb)
  }
  return this
}

function _destroy(
  self: Readable | Writable,
  err?: Error,
  cb?: (err?: Error | null) => void,
): void {
  let called = false

  function onDestroy(err?: Error | null): void {
    if (called) {
      return
    }
    called = true

    const r = self._readableState
    const w = self._writableState
    checkError(err, w, r)
    if (w) {
      w[kState] |= kClosed
    }
    if (r) {
      r[kState] |= kClosed
    }

    if (typeof cb === 'function') {
      cb(err)
    }

    if (err) {
      nextTick(emitErrorCloseNT, self, err)
    } else {
      nextTick(emitCloseNT, self)
    }
  }
  try {
    self._destroy(err || null, onDestroy)
  } catch (err) {
    onDestroy(err as Error)
  }
}

function emitErrorCloseNT(self: Readable | Writable, err: Error): void {
  emitErrorNT(self, err)
  emitCloseNT(self)
}

function emitCloseNT(self: Readable | Writable): void {
  const r = self._readableState
  const w = self._writableState
  if (w) {
    w[kState] |= kCloseEmitted
  }
  if (r) {
    r[kState] |= kCloseEmitted
  }
  if (
    (w && (w[kState] & kEmitClose) !== 0) ||
    (r && (r[kState] & kEmitClose) !== 0)
  ) {
    self.emit('close')
  }
}

function emitErrorNT(self: Readable | Writable, err: Error): void {
  const r = self._readableState
  const w = self._writableState
  if (
    (w && (w[kState] & kErrorEmitted) !== 0) ||
    (r && (r[kState] & kErrorEmitted) !== 0)
  ) {
    return
  }

  if (w) {
    w[kState] |= kErrorEmitted
  }
  if (r) {
    r[kState] |= kErrorEmitted
  }
  self.emit('error', err)
}

export function undestroy(this: Readable | Writable): void {
  const r = this._readableState
  const w = this._writableState
  if (r) {
    r.constructed = true
    r.closed = false
    r.closeEmitted = false
    r.destroyed = false
    r.errored = null
    r.errorEmitted = false
    r.reading = false
    r.ended = r.readable === false
    r.endEmitted = r.readable === false
  }
  if (w) {
    w.constructed = true
    w.destroyed = false
    w.closed = false
    w.closeEmitted = false
    w.errored = null
    w.errorEmitted = false
    w.finalCalled = false
    w.prefinished = false
    w.ended = w.writable === false
    w.ending = w.writable === false
    w.finished = w.writable === false
  }
}

export function errorOrDestroy(
  stream: Readable | Writable,
  err?: Error,
  sync: boolean = false,
  // @ts-expect-error TS2526 Apparently `this` is disallowed.
): this | undefined {
  // We have tests that rely on errors being emitted
  // in the same tick, so changing this is semver major.
  // For now when you opt-in to autoDestroy we allow
  // the error to be emitted nextTick. In a future
  // semver major update we should change the default to this.

  const r = stream._readableState
  const w = stream._writableState
  if (
    (w && (w[kState] ? (w[kState] & kDestroyed) !== 0 : w.destroyed)) ||
    (r && (r[kState] ? (r[kState] & kDestroyed) !== 0 : r.destroyed))
  ) {
    // @ts-expect-error TS2683 This should be somehow type-defined.
    return this
  }
  if (
    (r && (r[kState] & kAutoDestroy) !== 0) ||
    (w && (w[kState] & kAutoDestroy) !== 0)
  ) {
    stream.destroy(err)
  } else if (err) {
    // Avoid V8 leak, https://github.com/nodejs/node/pull/34103#issuecomment-652002364
    err.stack // eslint-disable-line @typescript-eslint/no-unused-expressions

    if (w && (w[kState] & kErrored) === 0) {
      w.errored = err
    }
    if (r && (r[kState] & kErrored) === 0) {
      r.errored = err
    }
    if (sync) {
      nextTick(emitErrorNT, stream, err)
    } else {
      emitErrorNT(stream, err)
    }
  }

  return undefined
}

export function construct(stream: Readable | Writable, cb: VoidFunction): void {
  if (typeof stream._construct !== 'function') {
    return
  }
  const r = stream._readableState
  const w = stream._writableState

  if (r) {
    r[kState] &= ~kConstructed
  }
  if (w) {
    w[kState] &= ~kConstructed
  }

  stream.once(kConstruct, cb)

  if (stream.listenerCount(kConstruct) > 1) {
    // Duplex
    return
  }

  nextTick(constructNT, stream)
}

function constructNT(this: unknown, stream: Readable | Writable): void {
  let called = false

  function onConstruct(err?: Error): void {
    if (called) {
      errorOrDestroy(stream, err ?? new ERR_MULTIPLE_CALLBACK())
      return
    }
    called = true

    const r = stream._readableState
    const w = stream._writableState
    const s = w || r

    if (r) {
      r[kState] |= kConstructed
    }
    if (w) {
      w[kState] |= kConstructed
    }

    if (s?.destroyed) {
      stream.emit(kDestroy, err)
    } else if (err) {
      errorOrDestroy(stream, err, true)
    } else {
      stream.emit(kConstruct)
    }
  }

  try {
    stream._construct?.((err) => {
      nextTick(onConstruct, err)
    })
  } catch (err) {
    nextTick(onConstruct, err)
  }
}

function isRequest(stream: unknown): stream is OutgoingMessage {
  return (
    stream != null &&
    typeof stream === 'object' &&
    'setHeader' in stream &&
    'abort' in stream &&
    typeof stream.abort === 'function'
  )
}

function emitCloseLegacy(stream: Readable | Writable): void {
  stream.emit('close')
}

function emitErrorCloseLegacy(stream: Readable | Writable, err?: Error): void {
  stream.emit('error', err)
  nextTick(emitCloseLegacy, stream)
}

// Normalize destroy for legacy.
export function destroyer(
  stream: Readable | Writable | null | undefined,
  err?: Error,
): void {
  if (!stream || isDestroyed(stream)) {
    return
  }

  if (!err && !isFinished(stream)) {
    err = new AbortError()
  }

  // TODO: Remove isRequest branches.
  if (isServerRequest(stream)) {
    // @ts-expect-error TS2540 - socket is read-only but we need to set it to null for cleanup
    stream.socket = null
    stream.destroy(err)
  } else if (isRequest(stream)) {
    // @ts-expect-error TS2339 - abort exists on OutgoingMessage but not in types
    stream.abort()
  } else if (isRequest(stream.req)) {
    // @ts-expect-error TS2339 - abort exists on req but not in all types
    stream.req.abort()
  } else if (typeof stream.destroy === 'function') {
    stream.destroy(err)
  } else if ('close' in stream && typeof stream.close === 'function') {
    // TODO: Don't lose err?
    stream.close()
  } else if (err) {
    nextTick(emitErrorCloseLegacy, stream, err)
  } else {
    nextTick(emitCloseLegacy, stream)
  }
  if (!stream.destroyed) {
    stream[kIsDestroyed] = true
  }
}
