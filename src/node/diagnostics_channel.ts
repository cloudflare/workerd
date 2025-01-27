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

import { default as diagnosticsChannel } from 'node-internal:diagnostics_channel';

import type {
  Channel as ChannelType,
  MessageCallback,
} from 'node-internal:diagnostics_channel';

import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';

import { validateObject } from 'node-internal:validators';

export const { Channel } = diagnosticsChannel;

export function hasSubscribers(name: string | symbol): boolean {
  return diagnosticsChannel.hasSubscribers(name);
}

export function channel(name: string | symbol): ChannelType {
  return diagnosticsChannel.channel(name);
}

export function subscribe(
  name: string | symbol,
  callback: MessageCallback
): void {
  diagnosticsChannel.subscribe(name, callback);
}

export function unsubscribe(
  name: string | symbol,
  callback: MessageCallback
): void {
  diagnosticsChannel.unsubscribe(name, callback);
}

export interface TracingChannelSubscriptions {
  start?: MessageCallback;
  end?: MessageCallback;
  asyncStart?: MessageCallback;
  asyncEnd?: MessageCallback;
  error?: MessageCallback;
}

export interface TracingChannels {
  start: ChannelType;
  end: ChannelType;
  asyncStart: ChannelType;
  asyncEnd: ChannelType;
  error: ChannelType;
}

const kStart = Symbol('kStart');
const kEnd = Symbol('kEnd');
const kAsyncStart = Symbol('kAsyncStart');
const kAsyncEnd = Symbol('kAsyncEnd');
const kError = Symbol('kError');

export class TracingChannel {
  private [kStart]?: ChannelType;
  private [kEnd]?: ChannelType;
  private [kAsyncStart]?: ChannelType;
  private [kAsyncEnd]?: ChannelType;
  private [kError]?: ChannelType;

  public constructor() {
    throw new Error(
      'Use diagnostic_channel.tracingChannels() to create TracingChannel'
    );
  }

  public get start(): ChannelType | undefined {
    return this[kStart];
  }
  public get end(): ChannelType | undefined {
    return this[kEnd];
  }
  public get asyncStart(): ChannelType | undefined {
    return this[kAsyncStart];
  }
  public get asyncEnd(): ChannelType | undefined {
    return this[kAsyncEnd];
  }
  public get error(): ChannelType | undefined {
    return this[kError];
  }

  public subscribe(subscriptions: TracingChannelSubscriptions): void {
    if (subscriptions.start !== undefined)
      this[kStart]?.subscribe(subscriptions.start);
    if (subscriptions.end !== undefined)
      this[kEnd]?.subscribe(subscriptions.end);
    if (subscriptions.asyncStart !== undefined)
      this[kAsyncStart]?.subscribe(subscriptions.asyncStart);
    if (subscriptions.asyncEnd !== undefined)
      this[kAsyncEnd]?.subscribe(subscriptions.asyncEnd);
    if (subscriptions.error !== undefined)
      this[kError]?.subscribe(subscriptions.error);
  }

  public unsubscribe(subscriptions: TracingChannelSubscriptions): void {
    if (subscriptions.start !== undefined)
      this[kStart]?.unsubscribe(subscriptions.start);
    if (subscriptions.end !== undefined)
      this[kEnd]?.unsubscribe(subscriptions.end);
    if (subscriptions.asyncStart !== undefined)
      this[kAsyncStart]?.unsubscribe(subscriptions.asyncStart);
    if (subscriptions.asyncEnd !== undefined)
      this[kAsyncEnd]?.unsubscribe(subscriptions.asyncEnd);
    if (subscriptions.error !== undefined)
      this[kError]?.unsubscribe(subscriptions.error);
  }

  public traceSync(
    fn: (...args: unknown[]) => unknown,
    context: Record<string, unknown> = {},
    thisArg: unknown = globalThis,
    ...args: unknown[]
  ): unknown {
    const { start, end, error } = this;

    return start?.runStores(
      context,
      () => {
        try {
          const result = Reflect.apply(fn, thisArg, args);
          context.result = result;
          return result;
        } catch (err) {
          context.error = err;
          error?.publish(context);
          throw err;
        } finally {
          end?.publish(context);
        }
      },
      thisArg
    );
  }

  public tracePromise(
    fn: (...args: unknown[]) => unknown,
    context: Record<string, unknown> = {},
    thisArg: unknown = globalThis,
    ...args: unknown[]
  ): unknown {
    const { start, end, asyncStart, asyncEnd, error } = this;

    function reject(err: Error): Promise<unknown> {
      context.error = err;
      error?.publish(context);
      asyncStart?.publish(context);
      asyncEnd?.publish(context);
      return Promise.reject(err);
    }

    function resolve(result: unknown): unknown {
      context.result = result;
      asyncStart?.publish(context);
      asyncEnd?.publish(context);
      return result;
    }

    return start?.runStores(
      context,
      () => {
        try {
          let promise = Reflect.apply(fn, thisArg, args) as Promise<unknown>;
          // Convert thenables to native promises
          if (!(promise instanceof Promise)) {
            promise = Promise.resolve(promise);
          }
          return promise.then(resolve, reject);
        } catch (err) {
          context.error = err;
          error?.publish(context);
          throw err;
        } finally {
          end?.publish(context);
        }
      },
      thisArg
    );
  }

  public traceCallback(
    fn: (...args: unknown[]) => unknown,
    position = -1,
    context: Record<string, unknown> = {},
    thisArg: unknown = globalThis,
    ...args: unknown[]
  ): unknown {
    const { start, end, asyncStart, asyncEnd, error } = this;

    function wrappedCallback(this: unknown, err: unknown, res: unknown): void {
      if (err) {
        context.error = err;
        error?.publish(context);
      } else {
        context.result = res;
      }

      // Using runStores here enables manual context failure recovery
      asyncStart?.runStores(
        context,
        () => {
          try {
            if (callback) {
              // eslint-disable-next-line prefer-rest-params
              Reflect.apply(callback, this, arguments);
            }
          } finally {
            asyncEnd?.publish(context);
          }
        },
        thisArg
      );
    }

    const callback = args[position] as VoidFunction | undefined;
    if (typeof callback !== 'function') {
      throw new ERR_INVALID_ARG_TYPE('callback', ['function'], callback);
    }
    args.splice(position, 1, wrappedCallback);

    return start?.runStores(
      context,
      () => {
        try {
          return Reflect.apply(fn, thisArg, args);
        } catch (err) {
          context.error = err;
          error?.publish(context);
          throw err;
        } finally {
          end?.publish(context);
        }
      },
      thisArg
    );
  }
}

function validateChannel(channel: unknown, name: string): ChannelType {
  if (!(channel instanceof Channel)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'Channel', channel);
  }
  return channel;
}

export function tracingChannel(
  nameOrChannels: string | TracingChannels
): TracingChannel {
  return Reflect.construct(
    function (this: TracingChannel) {
      if (typeof nameOrChannels === 'string') {
        this[kStart] = channel(`tracing:${nameOrChannels}:start`);
        this[kEnd] = channel(`tracing:${nameOrChannels}:end`);
        this[kAsyncStart] = channel(`tracing:${nameOrChannels}:asyncStart`);
        this[kAsyncEnd] = channel(`tracing:${nameOrChannels}:asyncEnd`);
        this[kError] = channel(`tracing:${nameOrChannels}:error`);
      } else {
        validateObject(nameOrChannels, 'channels', {});
        const channels = nameOrChannels as TracingChannels;
        this[kStart] = validateChannel(channels.start, 'channels.start');
        this[kEnd] = validateChannel(channels.end, 'channels.end');
        this[kAsyncStart] = validateChannel(
          channels.asyncStart,
          'channels.asyncStart'
        );
        this[kAsyncEnd] = validateChannel(
          channels.asyncEnd,
          'channels.asyncEnd'
        );
        this[kError] = validateChannel(channels.error, 'channels.error');
      }
    },
    [],
    TracingChannel
  ) as TracingChannel;
}

export default {
  hasSubscribers,
  channel,
  subscribe,
  unsubscribe,
  tracingChannel,
  Channel,
};
