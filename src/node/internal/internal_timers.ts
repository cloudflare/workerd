// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
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

import { validateFunction } from 'node-internal:validators';
import { default as timersUtil } from 'node-internal:timers';

let clearTimeoutImpl: (obj: Timeout) => void;

export class Timeout {
  #timer: number;
  #callback: (...args: unknown[]) => unknown;
  #after: number;
  #args: unknown[];
  #isRepeat: boolean;
  #isRefed: boolean;

  public constructor(
    callback: (...args: unknown[]) => unknown,
    after: number = 1,
    args: unknown[] = [],
    isRepeat: boolean = false,
    isRefed: boolean = false
  ) {
    this.#callback = callback;
    // Left it as multiply by 1 due to make the behavior as similar to Node.js
    // as possible.
    this.#after = after * 1;
    this.#args = args;
    this.#isRepeat = isRepeat;
    this.#isRefed = isRefed;
    this.#timer = this.#constructTimer();
  }

  #constructTimer(): number {
    // @ts-expect-error TS2322 Due to difference between Node.js and globals
    this.#timer = this.#isRepeat
      ? globalThis.setInterval(this.#callback, this.#after, ...this.#args)
      : globalThis.setTimeout(this.#callback, this.#after, ...this.#args);
    return this.#timer;
  }

  #clearTimeout(): void {
    if (this.#isRepeat) {
      globalThis.clearInterval(this.#timer);
    } else {
      globalThis.clearTimeout(this.#timer);
    }
  }

  public refresh(): this {
    this.#clearTimeout();
    this.#constructTimer();
    return this;
  }

  public unref(): this {
    // Intentionally left as no-op.
    this.#isRefed = false;
    return this;
  }

  public ref(): this {
    // Intentionally left as no-op.
    this.#isRefed = true;
    return this;
  }

  public hasRef(): boolean {
    return this.#isRefed;
  }

  public close(): this {
    this.#clearTimeout();
    return this;
  }

  public [Symbol.dispose](): void {
    this.#clearTimeout();
  }

  public [Symbol.toPrimitive](): number {
    return this.#timer;
  }

  static {
    clearTimeoutImpl = (obj: Timeout): void => {
      obj.#clearTimeout();
    };
  }
}

export function setTimeout<TArgs extends unknown[]>(
  callback: (...args: TArgs) => unknown,
  delay?: number,
  ...args: TArgs
): Timeout {
  validateFunction(callback, 'callback');

  return new Timeout(
    callback,
    delay,
    args,
    /* isRepeat */ false,
    /* isRefed */ true
  );
}

export function clearTimeout(
  timer: Timeout | string | number | undefined
): void {
  if (timer instanceof Timeout) {
    clearTimeoutImpl(timer);
  } else if (typeof timer === 'number') {
    globalThis.clearTimeout(timer);
  }
}

export const setImmediate = timersUtil.setImmediate.bind(timersUtil);

export const clearImmediate = timersUtil.clearImmediate.bind(timersUtil);

export function setInterval<TArgs extends unknown[]>(
  callback: (...args: TArgs) => void,
  repeat?: number,
  ...args: TArgs
): Timeout {
  validateFunction(callback, 'callback');
  return new Timeout(
    callback,
    repeat,
    args,
    /* isRepeat */ true,
    /* isRefed */ true
  );
}

export function clearInterval(
  timer: Timeout | string | number | undefined
): void {
  if (timer instanceof Timeout) {
    clearTimeoutImpl(timer);
  } else if (typeof timer === 'number') {
    globalThis.clearInterval(timer);
  }
}

/**
 * @deprecated Please use timeout.refresh() instead.
 */
export function active(timer: Timeout | string | number | undefined): void {
  if (timer instanceof Timeout) {
    timer.refresh();
  }
}

/**
 * @deprecated Please use clearTimeout instead.
 */
export function unenroll(timer: unknown): void {
  if (timer instanceof Timeout) {
    clearTimeoutImpl(timer);
  } else if (typeof timer === 'number') {
    globalThis.clearTimeout(timer);
  }
}

/**
 * @deprecated Please use setTimeout instead.
 */
export function enroll(_item: unknown, _msecs: number): void {
  throw new Error('Not implemented. Please use setTimeout() instead.');
}
