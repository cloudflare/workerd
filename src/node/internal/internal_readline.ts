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

import type Readline from 'node:readline'
import { EventEmitter } from 'node-internal:events'
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors'

export class Interface extends EventEmitter implements Readline.Interface {
  terminal: boolean
  line: string
  cursor: number

  constructor() {
    super()
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface')
  }

  getPrompt(): string {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.getPrompt')
  }

  setPrompt(_prompt: string): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.setPrompt')
  }

  prompt(_preserveCursor?: boolean): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.prompt')
  }

  question(_query: unknown, _options: unknown, _callback?: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.question')
  }

  pause(): this {
    return this
  }

  resume(): this {
    return this
  }

  close(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.close')
  }

  write(_data: unknown, _key?: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.write')
  }

  getCursorPos(): Readline.CursorPos {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface.getCursorPos')
  }

  [Symbol.dispose](): void {
    this.close()
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async [Symbol.asyncDispose](): Promise<void> {
    this.close()
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  [Symbol.asyncIterator](): NodeJS.AsyncIterator<string, undefined, any> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Interface[Symbol.asyncIterator]')
  }
}
