// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { AsyncLocalStorage } from 'node-internal:async_hooks';

export type TransformCallback = (value: any) => any;

export abstract class Channel {
  hasSubscribers(): boolean;
  publish(message: any): void;
  subscribe(callback: MessageCallback): void;
  unsubscribe(callback: MessageCallback): void;
  bindStore(
    context: AsyncLocalStorage<any>,
    transform?: TransformCallback
  ): void;
  unbindStore(context: AsyncLocalStorage<any>): void;
  runStores(
    context: any,
    fn: (...args: any[]) => any,
    receiver?: any,
    ...args: any[]
  ): any;
}

export type MessageCallback = (message: any, name: string | symbol) => void;
export function hasSubscribers(name: string | symbol): boolean;
export function channel(name: string | symbol): Channel;
export function subscribe(
  name: string | symbol,
  callback: MessageCallback
): void;
export function unsubscribe(
  name: string | symbol,
  callback: MessageCallback
): void;
