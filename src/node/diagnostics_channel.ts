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
import { default as asyncHooks } from 'node-internal:async_hooks';

import type {
  Channel as ChannelType,
  MessageCallback,
} from 'node-internal:diagnostics_channel';

import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';

import { validateObject } from 'node-internal:validators';

type TransformCallback = (value: unknown) => unknown;
type ChannelName = string | symbol;
type AsyncLocalStorage = InstanceType<typeof asyncHooks.AsyncLocalStorage>;

const hasSubscribersGetter =
  !!Cloudflare.compatibilityFlags['diagnostics_channel_has_subscribers_getter'];

// Called directly, so that a subclass overriding them changes nothing.
// eslint-disable-next-line @typescript-eslint/unbound-method
const { getStore: alsGetStore, run: alsRun } =
  asyncHooks.AsyncLocalStorage.prototype;

function illegalInvocation(): TypeError {
  return new TypeError(
    'Illegal invocation: function called with incorrect `this` reference. ' +
      'See https://developers.cloudflare.com/workers/observability/errors/' +
      '#illegal-invocation-errors for details.'
  );
}

function argumentTypeError(
  type: string,
  method: string,
  index: number,
  expected: string
): TypeError {
  return new TypeError(
    `Failed to execute '${method}' on '${type}': parameter ${index} is not of type '${expected}'.`
  );
}

// Symbols stay symbols; anything else is converted to a string.
function toName(name: unknown): ChannelName {
  return typeof name === 'symbol' ? name : String(name);
}

// Channels are keyed by the name's string form, so a symbol shares a channel with every other
// symbol of the same description, and with the string 'Symbol(description)'.
function registryKey(name: ChannelName): string {
  return typeof name === 'symbol'
    ? `Symbol(${String(name.description)})`
    : name;
}

function isAsyncLocalStorage(value: unknown): value is AsyncLocalStorage {
  if (typeof value !== 'object' || value === null) return false;
  try {
    // getStore() has no side effects, and throws unless `value` is a real AsyncLocalStorage.
    Reflect.apply(alsGetStore, value, []);
    return true;
  } catch {
    return false;
  }
}

// A map where erasing an entry moves the last entry into its place. This decides the order in
// which subscribers and stores run.
class Table<K, V> {
  #keys: K[] = [];
  #values: V[] = [];
  #index = new Map<K, number>();

  get size(): number {
    return this.#keys.length;
  }

  has(key: K): boolean {
    return this.#index.has(key);
  }

  set(key: K, value: V): void {
    const i = this.#index.get(key);
    if (i === undefined) {
      this.#index.set(key, this.#keys.length);
      this.#keys.push(key);
      this.#values.push(value);
    } else {
      this.#values[i] = value;
    }
  }

  delete(key: K): void {
    const i = this.#index.get(key);
    if (i === undefined) return;
    this.#index.delete(key);
    const lastKey = this.#keys.pop() as K;
    const lastValue = this.#values.pop() as V;
    if (i < this.#keys.length) {
      this.#keys[i] = lastKey;
      this.#values[i] = lastValue;
      this.#index.set(lastKey, i);
    }
  }

  keys(): K[] {
    return this.#keys.slice();
  }

  entries(): [K, V][] {
    return this.#keys.map((key, i) => [key, this.#values[i] as V]);
  }
}

const kCreate = Symbol('kCreate');

// Set in Channel's static block, so that the module functions below can read private state.
let hasSubscribersOf: (channel: Channel) => boolean;

export class Channel {
  readonly #name: ChannelName;
  readonly #subscribers = new Table<MessageCallback, undefined>();
  readonly #stores = new Table<
    AsyncLocalStorage,
    TransformCallback | undefined
  >();

  constructor(token: unknown, name: ChannelName) {
    if (token !== kCreate) throw new TypeError('Illegal constructor');
    this.#name = name;
  }

  static #check(channel: unknown): asserts channel is Channel {
    if (
      typeof channel !== 'object' ||
      channel === null ||
      !(#subscribers in channel)
    ) {
      throw illegalInvocation();
    }
  }

  #publish(message: unknown): void {
    for (const callback of this.#subscribers.keys()) {
      Reflect.apply(callback, globalThis, [message, this.#name]);
    }
    diagnosticsChannel.publishToTracer(this.#name, message);
  }

  // A getter instead, when the diagnostics_channel_has_subscribers_getter flag is on. See the
  // static block below.
  hasSubscribers(): boolean {
    Channel.#check(this);
    return this.#subscribers.size !== 0;
  }

  publish(...args: unknown[]): void {
    Channel.#check(this);
    if (args.length < 1) {
      throw argumentTypeError('Channel', 'publish', 1, 'Value');
    }
    this.#publish(args[0]);
  }

  subscribe(callback: unknown): void {
    Channel.#check(this);
    if (typeof callback !== 'function') {
      throw argumentTypeError('Channel', 'subscribe', 1, 'function');
    }
    if (!this.#subscribers.has(callback as MessageCallback)) {
      this.#subscribers.set(callback as MessageCallback, undefined);
    }
  }

  unsubscribe(callback: unknown): void {
    Channel.#check(this);
    if (typeof callback !== 'function') {
      throw argumentTypeError('Channel', 'unsubscribe', 1, 'function');
    }
    this.#subscribers.delete(callback as MessageCallback);
  }

  bindStore(store: unknown, transform?: unknown): void {
    Channel.#check(this);
    if (!isAsyncLocalStorage(store)) {
      throw argumentTypeError('Channel', 'bindStore', 1, 'AsyncLocalStorage');
    }
    if (transform !== undefined && typeof transform !== 'function') {
      throw argumentTypeError('Channel', 'bindStore', 2, 'function');
    }
    this.#stores.set(store, transform as TransformCallback | undefined);
  }

  unbindStore(store: unknown): void {
    Channel.#check(this);
    if (!isAsyncLocalStorage(store)) {
      throw argumentTypeError('Channel', 'unbindStore', 1, 'AsyncLocalStorage');
    }
    this.#stores.delete(store);
  }

  runStores(...args: unknown[]): unknown {
    Channel.#check(this);
    if (args.length < 1) {
      throw argumentTypeError('Channel', 'runStores', 1, 'Value');
    }
    const [message, callback, receiver] = args;
    if (typeof callback !== 'function') {
      throw argumentTypeError('Channel', 'runStores', 2, 'function');
    }
    const callbackArgs = args.slice(3);
    const thisArg = receiver === undefined ? globalThis : receiver;
    const stores = this.#stores.entries();

    // Enter each store in turn, so that a transform runs with the stores before it already set.
    const enter = (i: number): unknown => {
      const entry = stores[i];
      if (entry === undefined) {
        this.#publish(message);
        return Reflect.apply(callback, thisArg, callbackArgs);
      }
      const [store, transform] = entry;
      const value =
        transform === undefined
          ? message
          : Reflect.apply(transform, globalThis, [message]);
      return Reflect.apply(alsRun, store, [value, (): unknown => enter(i + 1)]);
    };
    return enter(0);
  }

  static {
    hasSubscribersOf = (channel): boolean => channel.#subscribers.size !== 0;

    // Built-in types get a Symbol.toStringTag with this flag, so Channel does too.
    if (Cloudflare.compatibilityFlags['set_tostring_tag']) {
      Object.defineProperty(this.prototype, Symbol.toStringTag, {
        value: 'Channel',
        writable: true,
        configurable: true,
        enumerable: false,
      });
    }

    if (hasSubscribersGetter) {
      Object.defineProperty(this.prototype, 'hasSubscribers', {
        get(this: Channel): boolean {
          Channel.#check(this);
          return this.#subscribers.size !== 0;
        },
        configurable: true,
        enumerable: false,
      });
    }
  }
}

// Channels are never removed.
const channels = new Map<string, Channel>();

function getChannel(name: ChannelName): Channel {
  const key = registryKey(name);
  let result = channels.get(key);
  if (result === undefined) {
    result = new Channel(kCreate, name);
    channels.set(key, result);
  }
  return result;
}

export function hasSubscribers(name: string | symbol): boolean {
  const channel = channels.get(registryKey(toName(name)));
  return channel !== undefined && hasSubscribersOf(channel);
}

export function channel(name: string | symbol): ChannelType {
  return getChannel(toName(name)) as unknown as ChannelType;
}

export function subscribe(
  name: string | symbol,
  callback: MessageCallback
): void {
  const channelName = toName(name);
  if (typeof callback !== 'function') {
    throw argumentTypeError(
      'DiagnosticsChannelModule',
      'subscribe',
      2,
      'function'
    );
  }
  getChannel(channelName).subscribe(callback);
}

export function unsubscribe(
  name: string | symbol,
  callback: MessageCallback
): void {
  const key = registryKey(toName(name));
  if (typeof callback !== 'function') {
    throw argumentTypeError(
      'DiagnosticsChannelModule',
      'unsubscribe',
      2,
      'function'
    );
  }
  channels.get(key)?.unsubscribe(callback);
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

  constructor() {
    throw new Error(
      'Use diagnostic_channel.tracingChannels() to create TracingChannel'
    );
  }

  get start(): ChannelType | undefined {
    return this[kStart];
  }
  get end(): ChannelType | undefined {
    return this[kEnd];
  }
  get asyncStart(): ChannelType | undefined {
    return this[kAsyncStart];
  }
  get asyncEnd(): ChannelType | undefined {
    return this[kAsyncEnd];
  }
  get error(): ChannelType | undefined {
    return this[kError];
  }

  // `hasSubscribers` is installed on the prototype below, gated by the
  // `diagnostics_channel_has_subscribers_getter` compatibility flag. When the
  // flag is enabled it's a getter property (matching Node.js); when disabled
  // it's a method (preserving the original workerd behavior).
  static {
    if (hasSubscribersGetter) {
      Object.defineProperty(this.prototype, 'hasSubscribers', {
        get(this: TracingChannel): boolean {
          return [
            this.start,
            this.end,
            this.asyncStart,
            this.asyncEnd,
            this.error,
          ].some((c) => c != null && (c.hasSubscribers as unknown as boolean));
        },
        configurable: true,
        enumerable: false,
      });
    } else {
      Object.defineProperty(this.prototype, 'hasSubscribers', {
        value: function hasSubscribers(this: TracingChannel): boolean {
          return [
            this.start,
            this.end,
            this.asyncStart,
            this.asyncEnd,
            this.error,
          ].some(
            (c) =>
              c != null &&
              (c.hasSubscribers as unknown as () => boolean).call(c)
          );
        },
        writable: true,
        configurable: true,
        enumerable: false,
      });
    }
  }

  subscribe(subscriptions: TracingChannelSubscriptions): void {
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

  unsubscribe(subscriptions: TracingChannelSubscriptions): void {
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

  traceSync(
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

  tracePromise(
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

  traceCallback(
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
  return channel as unknown as ChannelType;
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
        validateObject(nameOrChannels, 'channels');
        this[kStart] = validateChannel(nameOrChannels.start, 'channels.start');
        this[kEnd] = validateChannel(nameOrChannels.end, 'channels.end');
        this[kAsyncStart] = validateChannel(
          nameOrChannels.asyncStart,
          'channels.asyncStart'
        );
        this[kAsyncEnd] = validateChannel(
          nameOrChannels.asyncEnd,
          'channels.asyncEnd'
        );
        this[kError] = validateChannel(nameOrChannels.error, 'channels.error');
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
