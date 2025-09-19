// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { EventEmitter } from 'node-internal:events';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import type {
  InspectorConsole,
  Session as _Session,
  Network as _Network,
} from 'node:inspector';

export function close(): void {
  // Acts as a no-op.
}

const noop: VoidFunction = () => {};

export const console: InspectorConsole = {
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

export function open(
  _port?: number,
  _host?: string,
  _wait?: boolean
): Disposable {
  return {
    [Symbol.dispose](): Promise<void> {
      return Promise.resolve();
    },
  };
}

export function url(): string | undefined {
  return undefined;
}

export function waitForDebugger(): void {
  // Acts as a no-op.
}

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

  post(_method: unknown, _params?: unknown, _callback?: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Session.post');
  }
}

export const Network: typeof _Network = {
  requestWillBeSent(_params: _Network.RequestWillBeSentEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.requestWillBeSent');
  },
  dataReceived(_params: _Network.DataReceivedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.dataReceived');
  },
  dataSent(_params: unknown): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.dataSent');
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
};

export default {
  Session,
  close,
  console,
  open,
  url,
  waitForDebugger,
  Network,
};
