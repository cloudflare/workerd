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

import { EventEmitter } from 'node-internal:events';
import type { Abortable } from 'node:events';
import type Readline from 'node:readline';

// This class provides a no-op stub implementation that matches unenv's behavior.
// See: https://github.com/unjs/unenv/blob/main/src/runtime/node/internal/readline/interface.ts
// Methods are no-ops or return sensible defaults rather than throwing errors,
// which allows code that depends on readline to work without crashing.
export class Interface extends EventEmitter implements Readline.Interface {
  terminal = false;
  line = '';
  cursor = 0;

  getPrompt(): string {
    return '';
  }

  setPrompt(_prompt: string): void {
    // No-op
  }

  prompt(_preserveCursor?: boolean): void {
    // No-op
  }

  question(query: string, callback: (answer: string) => void): void;
  question(
    query: string,
    options: Abortable,
    callback: (answer: string) => void
  ): void;
  question(
    _query: string,
    _optionsOrCallback: Abortable | ((answer: string) => void),
    _callback?: (answer: string) => void
  ): void {
    // If callback is the second argument (no options)
    if (typeof _optionsOrCallback === 'function') {
      _optionsOrCallback('');
    } else if (typeof _callback === 'function') {
      _callback('');
    }
  }

  pause(): this {
    return this;
  }

  resume(): this {
    return this;
  }

  close(): void {
    // No-op
  }

  write(_data: unknown, _key?: unknown): void {
    // No-op
  }

  getCursorPos(): Readline.CursorPos {
    return {
      rows: 0,
      cols: 0,
    };
  }

  [Symbol.dispose](): void {
    this.close();
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  async [Symbol.asyncDispose](): Promise<void> {
    this.close();
  }

  // Yield a single empty string so that `for await...of` loops complete
  // immediately without blocking, consistent with no-op stub behavior.
  async *[Symbol.asyncIterator](): NodeJS.AsyncIterator<string> {
    yield '';
  }
}
