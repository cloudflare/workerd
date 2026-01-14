// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import NodeJSStream from 'node:stream'
import EventEmitter from 'node:events'

export declare class Stream extends NodeJSStream.Stream {
  static isWritable: typeof NodeJSStream.isWritable
  static isDistributed: typeof NodeJSStream.isDistributed
  static compose: typeof NodeJSStream.compose
  static destroy: typeof NodeJSStream.destroy
  static finished: typeof NodeJSStream.finished
  static pipeline: typeof NodeJSStream.pipeline
  static isDisturbed: typeof NodeJSStream.isDisturbed
  static isDestroyed: typeof NodeJSStream.isDestroyed

  static isReadable(stream: NodeStreamLike): boolean | null
  static _isArrayBufferView(value: unknown): value is ArrayBufferView
  static _isUint8Array(value: unknown): value is Uint8Array
  static _uint8ArrayToBuffer(value: Uint8Array): NodeJS.Buffer

  static Stream: typeof Stream

  aborted?: boolean
  readable: boolean
  writable: boolean
  destroyed: boolean
  req: EventEmitter

  _writableState: {
    errorEmitted: boolean
    pendingcb?: number
  }
  _readableState: {
    errorEmitted: boolean
  }
}
