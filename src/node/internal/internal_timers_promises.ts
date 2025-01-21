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

import * as timers from 'node-internal:internal_timers';
import { ERR_INVALID_THIS, AbortError } from 'node-internal:internal_errors';
import {
  validateNumber,
  validateAbortSignal,
  validateBoolean,
  validateObject,
} from 'node-internal:validators';

const kScheduler = Symbol.for('kScheduler');

export async function setTimeout<T = void>(
  delay: number | undefined,
  value?: T,
  options: { signal?: AbortSignal; ref?: boolean } = {}
): Promise<T> {
  if (delay !== undefined) {
    validateNumber(delay, 'delay');
  }

  validateObject(options, 'options');

  // Ref options is a no-op.
  const { signal, ref } = options;

  if (signal !== undefined) {
    validateAbortSignal(signal, 'options.signal');
  }

  // This is required due to consistency/compat reasons, even if it's no-op.
  if (ref !== undefined) {
    validateBoolean(ref, 'options.ref');
  }

  if (signal?.aborted) {
    throw new AbortError(undefined, { cause: signal.reason });
  }

  const { promise, resolve, reject } = Promise.withResolvers<T>();

  const timer = timers.setTimeout(() => {
    resolve(value as T);
  }, delay ?? 0);

  if (signal) {
    function onCancel(): void {
      timers.clearTimeout(timer);
      reject(new AbortError(undefined, { cause: signal?.reason }));
    }
    signal.addEventListener('abort', onCancel);
  }

  return promise;
}

export async function setImmediate<T>(
  value?: T,
  options: { signal?: AbortSignal; ref?: boolean } = {}
): Promise<T> {
  validateObject(options, 'options');

  // Ref options is a no-op.
  const { signal, ref } = options;

  if (signal !== undefined) {
    validateAbortSignal(signal, 'options.signal');
  }

  // This is required due to consistency/compat reasons, even if it's no-op.
  if (ref !== undefined) {
    validateBoolean(ref, 'options.ref');
  }

  if (signal?.aborted) {
    throw new AbortError(undefined, { cause: signal.reason });
  }

  const { promise, resolve, reject } = Promise.withResolvers<T>();

  const timer = timers.setImmediate(() => {
    resolve(value as T);
  });

  if (signal) {
    function onCancel(): void {
      timers.clearImmediate(timer);
      signal?.removeEventListener('abort', onCancel);
      reject(new AbortError(undefined, { cause: signal?.reason }));
    }
    signal.addEventListener('abort', onCancel);
  }

  return promise;
}

export async function* setInterval<T = void>(
  delay?: number,
  value?: T,
  options: { signal?: AbortSignal; ref?: boolean } = {}
): AsyncGenerator<T> {
  if (delay !== undefined) {
    validateNumber(delay, 'delay');
  }

  validateObject(options, 'options');

  // Ref options is a no-op.
  const { signal, ref } = options;

  if (signal !== undefined) {
    validateAbortSignal(signal, 'options.signal');
  }

  if (ref !== undefined) {
    validateBoolean(ref, 'options.ref');
  }

  if (signal?.aborted) {
    throw new AbortError(undefined, { cause: signal.reason });
  }

  let onCancel: (() => void) | undefined;
  let interval: timers.Timeout;
  try {
    let notYielded = 0;
    let callback: ((promise?: Promise<void>) => void) | undefined;
    interval = new timers.Timeout(
      () => {
        notYielded++;
        callback?.();
        callback = undefined;
      },
      delay,
      undefined,
      true,
      ref
    );

    if (signal) {
      onCancel = (): void => {
        timers.clearInterval(interval);
        // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
        signal.removeEventListener('abort', onCancel!);
        callback?.(
          Promise.reject(new AbortError(undefined, { cause: signal.reason }))
        );
        callback = undefined;
      };
      signal.addEventListener('abort', onCancel);
    }

    while (!signal?.aborted) {
      if (notYielded === 0) {
        await new Promise((resolve) => (callback = resolve));
      }
      for (; notYielded > 0; notYielded--) {
        yield value as T;
      }
    }
    throw new AbortError(undefined, { cause: signal.reason });
  } finally {
    // @ts-expect-error TS2454 TS detects invalid use before assignment.
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    if (interval) {
      timers.clearInterval(interval);
    }
    if (onCancel) {
      signal?.removeEventListener('abort', onCancel);
    }
  }
}

declare global {
  // eslint-disable-next-line no-var
  var scheduler: {
    wait: (delay: number, options?: { signal?: AbortSignal }) => Promise<void>;
  };
}

// TODO(@jasnell): Scheduler is an API currently being discussed by WICG
// for Web Platform standardization: https://github.com/WICG/scheduling-apis
// The scheduler.yield() and scheduler.wait() methods correspond roughly to
// the awaitable setTimeout and setImmediate implementations here. This api
// should be considered to be experimental until the spec for these are
// finalized. Note, also, that Scheduler is expected to be defined as a global,
// but while the API is experimental we shouldn't expose it as such.
class Scheduler {
  public [kScheduler] = true;

  public yield(): Promise<void> {
    if (!this[kScheduler]) throw new ERR_INVALID_THIS('Scheduler');
    return setImmediate();
  }

  public wait(
    ...args: Parameters<typeof globalThis.scheduler.wait>
  ): Promise<void> {
    if (!this[kScheduler]) throw new ERR_INVALID_THIS('Scheduler');
    return globalThis.scheduler.wait(...args);
  }
}

export const scheduler = new Scheduler();
