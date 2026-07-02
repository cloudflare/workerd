// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { EventEmitter } from 'node-internal:events';
import { PromiseSession as RealSession } from 'node-internal:internal_inspector';
import type {
  console as _console,
  Network as _Network,
  Session as _Session,
} from 'node:inspector/promises';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

// EXPERIMENTAL, LOCAL-DEV-ONLY. When this flag is set (only possible under `workerd --experimental`,
// never in production), `Session` connects to the isolate's own V8 inspector instead of throwing.
// See compatibility-date.capnp ("enable_nodejs_inspector_local_dev") for details.
const realInspectorEnabled =
  !!Cloudflare.compatibilityFlags['enable_nodejs_inspector_local_dev'];

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

// The DevTools "Integration with DevTools" helpers (Network.*) inject Node-synthetic CDP events
// into the inspector channel (feeding any connected Session, including an in-process one). They are
// intentionally left as throwing stubs: this module's supported scope is *issuing commands* to the
// V8 inspector backend (e.g. the Profiler domain for code coverage), not event injection. V8 has no
// Network/DOMStorage domains, so implementing these would need separate host-side plumbing behind
// its own flag.
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
  webSocketCreated(_params: _Network.WebSocketCreatedEventDataType): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Network.webSocketCreated');
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
};

// Non-functional stub used when the experimental flag is off (the default, and the only behavior
// possible in production). Preserves the historical throwing behavior.
class StubSession extends EventEmitter implements _Session {
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

// intentional: conditional class export requires a structural cast. Both RealSession and
// StubSession implement the _Session interface; this is a TypeScript export-shape assertion to
// present the public node:inspector/promises Session type, not a runtime cast of untrusted/external
// data.
export const Session = (realInspectorEnabled
  ? RealSession
  : StubSession) as unknown as typeof _Session;

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
