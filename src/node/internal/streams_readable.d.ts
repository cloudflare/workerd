// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Readable as _Readable, Duplex } from 'node:stream'
import type EventEmitter from 'node:events'
import {
  kState,
  kIsWritable,
  kIsReadable,
  kIsClosedPromise,
  kIsDestroyed,
} from 'node-internal:streams_util'

export declare class ReadableState {
  readable?: boolean
  ended?: boolean
  endEmitted?: boolean
  reading?: boolean
  constructed?: boolean
  closed?: boolean
  closeEmitted?: boolean
  destroyed?: boolean
  errored?: Error | null
  errorEmitted?: boolean
  length?: number
  highWaterMark?: number
  autoDestroy?: boolean
  emitClose?: boolean
  readingMore?: boolean
  readableDidRead?: boolean
  readableAborted?: boolean;
  [kState]: number
}

export declare class Readable extends _Readable {
  _readableState: ReadableState
  _writableState: undefined
  _closed: boolean
  errored?: Error | null
  writableErrored: boolean
  readableErrored: boolean
  writableFinished: undefined
  readableFinished?: boolean
  readable: boolean
  writable?: boolean
  aborted?: boolean
  req?: EventEmitter

  writableEnded?: boolean;

  [kIsWritable]: boolean;
  [kIsReadable]: boolean;
  [kIsClosedPromise]: Promise<unknown>;
  [kIsDestroyed]: boolean
}

export const from: typeof Duplex.from
export const fromWeb: typeof Duplex.fromWeb
export const toWeb: typeof Duplex.toWeb
export const wrap: typeof Duplex.wrap
