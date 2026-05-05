// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

import { default as async_hooks } from 'node-internal:async_hooks';

class AsyncHook {
  enable(): this {
    return this;
  }

  disable(): this {
    return this;
  }
}

export const { AsyncLocalStorage, AsyncResource } = async_hooks;

// Async wrap provider type constants matching Node.js's ProviderType enum in
// async_wrap.h. While we don't use most of these internally, they are exported
// for compatibility with code that references them.
export const asyncWrapProviders: Record<string, number> = {
  NONE: 0,
  DIRHANDLE: 0,
  DNSCHANNEL: 0,
  ELDHISTOGRAM: 0,
  FILEHANDLE: 0,
  FILEHANDLECLOSEREQ: 0,
  BLOBREADER: 0,
  FSEVENTWRAP: 0,
  FSREQCALLBACK: 0,
  FSREQPROMISE: 0,
  GETADDRINFOREQWRAP: 0,
  GETNAMEINFOREQWRAP: 0,
  HEAPSNAPSHOT: 0,
  HTTP2SESSION: 0,
  HTTP2STREAM: 0,
  HTTP2PING: 0,
  HTTP2SETTINGS: 0,
  HTTPINCOMINGMESSAGE: 0,
  HTTPCLIENTREQUEST: 0,
  LOCKS: 0,
  JSSTREAM: 0,
  JSUDPWRAP: 0,
  MESSAGEPORT: 0,
  PIPECONNECTWRAP: 0,
  PIPESERVERWRAP: 0,
  PIPEWRAP: 0,
  PROCESSWRAP: 0,
  PROMISE: 0,
  QUERYWRAP: 0,
  QUIC_ENDPOINT: 0,
  QUIC_LOGSTREAM: 0,
  QUIC_SESSION: 0,
  QUIC_STREAM: 0,
  QUIC_UDP: 0,
  SHUTDOWNWRAP: 0,
  SIGNALWRAP: 0,
  STATWATCHER: 0,
  STREAMPIPE: 0,
  TCPCONNECTWRAP: 0,
  TCPSERVERWRAP: 0,
  TCPWRAP: 0,
  TTYWRAP: 0,
  UDPSENDWRAP: 0,
  UDPWRAP: 0,
  SIGINTWATCHDOG: 0,
  WORKER: 0,
  WORKERCPUPROFILE: 0,
  WORKERCPUUSAGE: 0,
  WORKERHEAPPROFILE: 0,
  WORKERHEAPSNAPSHOT: 0,
  WORKERHEAPSTATISTICS: 0,
  WRITEWRAP: 0,
  ZLIB: 0,
  CHECKPRIMEREQUEST: 0,
  PBKDF2REQUEST: 0,
  KEYPAIRGENREQUEST: 0,
  KEYGENREQUEST: 0,
  KEYEXPORTREQUEST: 0,
  ARGON2REQUEST: 0,
  CIPHERREQUEST: 0,
  DERIVEBITSREQUEST: 0,
  HASHREQUEST: 0,
  RANDOMBYTESREQUEST: 0,
  RANDOMPRIMEREQUEST: 0,
  SCRYPTREQUEST: 0,
  SIGNREQUEST: 0,
  TLSWRAP: 0,
  VERIFYREQUEST: 0,
  QUIC_PACKET: 0,
};

export function createHook(): AsyncHook {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  return new AsyncHook();
}

export function executionAsyncId(): number {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  return 0;
}

export function executionAsyncResource(): Record<string, string> {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  // eslint-disable-next-line @typescript-eslint/no-unsafe-return
  return Object.create(null);
}

export function triggerAsyncId(): number {
  // Even though we don't implement this function, we return a default value
  // in order to preserve backward compatibility and avoid breaking changes
  // with unenv polyfills.
  return 0;
}

export default {
  AsyncLocalStorage,
  AsyncResource,
  asyncWrapProviders,
  createHook,
  executionAsyncId,
  executionAsyncResource,
  triggerAsyncId,
};
