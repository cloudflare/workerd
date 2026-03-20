// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { EventEmitter } from 'node-internal:events';
import type {
  console as _console,
  Network as _Network,
  Session as _Session,
} from 'node:inspector/promises';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

const noop: VoidFunction = () => {};
export const console: typeof _console = {
  debug: noop,
  error: noop,
  info: noop,
  log: noop,
  warn: noop,
  dir: noop,
  dirxml: noop,
  table: noop,
  trace: noop,
  group: noop,
  groupCollapsed: noop,
  groupEnd: noop,
  clear: noop,
  count: noop,
  countReset: noop,
  assert: noop,
  profile: noop,
  profileEnd: noop,
  time: noop,
  timeLog: noop,
  timeStamp: noop,
};

export const Network: typeof _Network = {
  requestWillBeSent(_params: _Network.RequestWillBeSentEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.requestWillBeSent');
  },
  dataReceived(_params: _Network.DataReceivedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.dataReceived');
  },
  responseReceived(_params: _Network.ResponseReceivedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.responseReceived');
  },
  loadingFinished(_params: _Network.LoadingFinishedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.loadingFinished');
  },
  loadingFailed(_params: _Network.LoadingFailedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.loadingFailed');
  },
  webSocketHandshakeResponseReceived(
    _params: _Network.WebSocketHandshakeResponseReceivedEventDataType
  ): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED(
      'Network.webSocketHandshakeResponseReceived'
    );
  },
  webSocketClosed(_params: _Network.WebSocketClosedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.webSocketClosed');
  },
  webSocketCreated(_params: _Network.WebSocketCreatedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.webSocketCreated');
  },
  dataSent(_params: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.dataSent');
  },
};

export class Session extends EventEmitter implements _Session {
  constructor() {
    super();
    throw new ERR_METHOD_NOT_IMPLEMENTED('Session');
  }

  connect(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Session.connect');
  }

  connectToMainThread(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Session.connectToMainThread');
  }

  disconnect(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Session.disconnect');
  }

  // @ts-expect-error TS2416 This is intentional
  post(
    _method: unknown,
    _params?: unknown,
    _callback?: unknown
  ): Promise<unknown> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Session.post');
  }
}

export function url(): string | undefined {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Inspector.url');
}

export function waitForDebugger(): Promise<void> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Inspector.waitForDebugger');
}

export function open(): Disposable {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Inspector.open');
}

export function close(): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('Inspector.close');
}

export default {
  close,
  console,
  Network,
  open,
  Session,
  url,
  waitForDebugger,
};
