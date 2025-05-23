// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { ok } from 'node-internal:internal_assert';
import { _checkIsHttpToken as checkIsHttpToken } from 'node-internal:internal_http';
import {
  kUniqueHeaders,
  parseUniqueHeadersOption,
  OutgoingMessage,
  kOutHeaders,
  kNeedDrain,
} from 'node-internal:internal_http_outgoing';
import { Buffer } from 'node-internal:internal_buffer';
import { urlToHttpOptions, isURL } from 'node-internal:internal_url';
import { once } from 'node-internal:internal_http_util';
import {
  ConnResetException,
  ERR_HTTP_HEADERS_SENT,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_HTTP_TOKEN,
  ERR_INVALID_PROTOCOL,
  ERR_UNESCAPED_CHARACTERS,
} from 'node-internal:internal_errors';
import { validateInteger, validateBoolean } from 'node-internal:validators';
import { getTimerDuration } from 'node-internal:internal_net';
import { addAbortSignal, finished } from 'node-internal:streams_util';
import { globalAgent, Agent } from 'node-internal:internal_http_agent';
import type {
  ClientRequest as _ClientRequest,
  RequestOptions as _RequestOptions,
} from 'node:http';

function emitErrorEvent(request: any, error: any): void {
  request.emit('error', error);
}

const INVALID_PATH_REGEX = /[^\u0021-\u00ff]/;
const kError = Symbol('kError');

function validateHost(host: unknown, name: string): string {
  if (host !== null && host !== undefined && typeof host !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(
      `options.${name}`,
      ['string', 'undefined', 'null'],
      host
    );
  }
  return host as string;
}

type Callback = (err: Error | null) => void;

export type RequestOptions = _RequestOptions & {
  _defaultAgent?: Agent;
};

// @ts-expect-error TS2323 Redeclare error.
export declare class ClientRequest extends OutgoingMessage {
  // Properties from constructor
  public agent: Agent;
  public socketPath?: string | undefined;
  public timeout?: number;
  public method: string;
  public maxHeaderSize?: number | undefined;
  public insecureHTTPParser?: boolean | undefined;
  public joinDuplicateHeaders?: boolean | undefined;
  public path: string;
  public _ended: boolean;
  public res: any;
  public aborted: boolean;
  public timeoutCb: any;
  public upgradeOrConnect: boolean;
  public parser: any;
  public maxHeadersCount: number | null;
  public reusedSocket: boolean;
  public host: string;
  public protocol: string;
  public _traceEventId?: number;
  public [kError]?: Error | undefined | null;
  public _httpMessage?: ClientRequest | undefined;

  // Methods that ClientRequest-specific (not inherited from OutgoingMessage)
  public abort(): void;
  public onSocket(socket: any): void;
  public setNoDelay(noDelay?: boolean): void;
  public setSocketKeepAlive(enable?: boolean, initialDelay?: number): void;
  public clearTimeout(cb?: () => void): void;
  public _deferToConnect(method: string, arguments_: any[]): void;

  public constructor(input: Callback);
  public constructor(
    input: string | URL | Record<string, unknown> | null,
    options?: Callback
  );
  public constructor(
    input: string | URL | Record<string, unknown> | null,
    options?: _RequestOptions,
    cb?: Callback
  );
}

// @ts-expect-error TS2323 Redeclare error.
export function ClientRequest(
  this: ClientRequest,
  input: (string | URL | Record<string, unknown> | null) | _RequestOptions,
  options?: _RequestOptions | Callback,
  cb?: Callback
) {
  // @ts-expect-error TS2559 Typescript fails to understand constructor arguments.
  OutgoingMessage.call(this as unknown as OutgoingMessage, []);

  if (typeof input === 'string') {
    const urlStr = input;
    input = urlToHttpOptions(new URL(urlStr));
  } else if (isURL(input)) {
    // url.URL instance
    input = urlToHttpOptions(input);
  } else {
    cb = options as Callback;
    options = input as _RequestOptions;
    input = null;
  }

  if (typeof options === 'function') {
    cb = options;
    options = input || {};
  } else {
    options = Object.assign(input || {}, options);
  }

  let agent = options.agent as Agent | undefined | boolean;
  const defaultAgent = (options._defaultAgent || globalAgent) as Agent;
  if (agent === false) {
    // @ts-expect-error TS2379
    agent = new defaultAgent.constructor();
  } else if (agent == null) {
    if (typeof options.createConnection !== 'function') {
      agent = defaultAgent as Agent;
    }
    // Explicitly pass through this statement as agent will not be used
    // when createConnection is provided.
  } else if (typeof (agent as Agent).addRequest !== 'function') {
    throw new ERR_INVALID_ARG_TYPE(
      'options.agent',
      ['Agent-like Object', 'undefined', 'false'],
      agent
    );
  }
  this.agent = agent as Agent;

  const protocol = options.protocol || defaultAgent.protocol;
  let expectedProtocol = defaultAgent.protocol;
  if (this.agent?.protocol) expectedProtocol = this.agent.protocol;

  if (options.path) {
    const path = String(options.path);
    if (INVALID_PATH_REGEX.test(path)) {
      throw new ERR_UNESCAPED_CHARACTERS('Request path');
    }
  }

  if (protocol !== expectedProtocol) {
    throw new ERR_INVALID_PROTOCOL(protocol, expectedProtocol);
  }

  const defaultPort = options.defaultPort || this.agent?.defaultPort;

  const optsWithoutSignal = { __proto__: null, ...options };

  const port = (optsWithoutSignal.port = options.port || defaultPort || 80);
  const host = (optsWithoutSignal.host =
    validateHost(options.hostname, 'hostname') ||
    validateHost(options.host, 'host') ||
    'localhost');

  const setHost =
    options.setHost !== undefined
      ? Boolean(options.setHost)
      : options.setDefaultHeaders !== false;

  this._removedConnection = options.setDefaultHeaders === false;
  this._removedContLen = options.setDefaultHeaders === false;
  this._removedTE = options.setDefaultHeaders === false;

  this.socketPath = options.socketPath;

  if (options.timeout !== undefined)
    this.timeout = getTimerDuration(options.timeout, 'timeout');

  const signal = options.signal;
  if (signal) {
    addAbortSignal(signal, this);
    delete optsWithoutSignal.signal;
  }
  let method = options.method;
  const methodIsString = typeof method === 'string';
  if (method !== null && method !== undefined && !methodIsString) {
    throw new ERR_INVALID_ARG_TYPE('options.method', 'string', method);
  }

  if (methodIsString && method) {
    if (!checkIsHttpToken(method)) {
      throw new ERR_INVALID_HTTP_TOKEN('Method', method);
    }
    method = this.method = method.toUpperCase();
  } else {
    method = this.method = 'GET';
  }

  const maxHeaderSize = options.maxHeaderSize;
  if (maxHeaderSize !== undefined)
    validateInteger(maxHeaderSize, 'maxHeaderSize', 0);
  this.maxHeaderSize = maxHeaderSize;

  const insecureHTTPParser = options.insecureHTTPParser;
  if (insecureHTTPParser !== undefined) {
    validateBoolean(insecureHTTPParser, 'options.insecureHTTPParser');
  }

  this.insecureHTTPParser = insecureHTTPParser;

  if (options.joinDuplicateHeaders !== undefined) {
    validateBoolean(
      options.joinDuplicateHeaders,
      'options.joinDuplicateHeaders'
    );
  }

  this.joinDuplicateHeaders = options.joinDuplicateHeaders;

  this.path = options.path || '/';
  if (cb) {
    this.once('response', cb);
  }

  if (
    method === 'GET' ||
    method === 'HEAD' ||
    method === 'DELETE' ||
    method === 'OPTIONS' ||
    method === 'TRACE' ||
    method === 'CONNECT'
  ) {
    this.useChunkedEncodingByDefault = false;
  } else {
    this.useChunkedEncodingByDefault = true;
  }

  this._ended = false;
  this.res = null;
  this.aborted = false;
  this.timeoutCb = null;
  this.upgradeOrConnect = false;
  this.parser = null;
  this.maxHeadersCount = null;
  this.reusedSocket = false;
  this.host = host;
  this.protocol = protocol;

  if (this.agent) {
    // If there is an agent we should default to Connection:keep-alive,
    // but only if the Agent will actually reuse the connection!
    // If it's not a keepAlive agent, and the maxSockets==Infinity, then
    // there's never a case where this socket will actually be reused
    if (!this.agent.keepAlive && !Number.isFinite(this.agent.maxSockets)) {
      this._last = true;
      this.shouldKeepAlive = false;
    } else {
      this._last = false;
      this.shouldKeepAlive = true;
    }
  }

  const headersArray = Array.isArray(options.headers);
  if (!headersArray) {
    if (options.headers) {
      const keys = Object.keys(options.headers);
      // Retain for(;;) loop for performance reasons
      // Refs: https://github.com/nodejs/node/pull/30958
      for (let i = 0; i < keys.length; i++) {
        const key = keys[i] as string;
        this.setHeader(key, options.headers[key] as string);
      }
    }

    if (host && !this.getHeader('host') && setHost) {
      let hostHeader = host;

      // For the Host header, ensure that IPv6 addresses are enclosed
      // in square brackets, as defined by URI formatting
      // https://tools.ietf.org/html/rfc3986#section-3.2.2
      const posColon = hostHeader.indexOf(':');
      if (
        posColon !== -1 &&
        hostHeader.includes(':', posColon + 1) &&
        hostHeader.charCodeAt(0) !== 91 /* '[' */
      ) {
        hostHeader = `[${hostHeader}]`;
      }

      if (port && +port !== defaultPort) {
        hostHeader += ':' + port;
      }
      this.setHeader('Host', hostHeader as string);
    }

    if (options.auth && !this.getHeader('Authorization')) {
      this.setHeader(
        'Authorization',
        'Basic ' + Buffer.from(options.auth).toString('base64')
      );
    }

    if (this.getHeader('expect')) {
      if (this._header) {
        throw new ERR_HTTP_HEADERS_SENT('render');
      }

      this._storeHeader(
        this.method + ' ' + this.path + ' HTTP/1.1\r\n',
        this[kOutHeaders]
      );
    }
  } else {
    this._storeHeader(
      this.method + ' ' + this.path + ' HTTP/1.1\r\n',
      options.headers as unknown as Record<string, [string, string | string[]]>
    );
  }

  this[kUniqueHeaders] = parseUniqueHeadersOption(options.uniqueHeaders);

  // initiate connection
  if (this.agent) {
    // @ts-expect-error TS2379 Type inconsistencies
    this.agent.addRequest(this, optsWithoutSignal);
  } else {
    // No agent, default to Connection:close.
    this._last = true;
    this.shouldKeepAlive = false;
    let opts = optsWithoutSignal;
    if (opts.path || opts.socketPath) {
      opts = { ...optsWithoutSignal };
      if (opts.socketPath) {
        opts.path = opts.socketPath;
      } else {
        opts.path &&= undefined;
      }
    }
    if (typeof opts.createConnection === 'function') {
      const oncreate = once((err, socket) => {
        if (err) {
          process.nextTick(() => emitErrorEvent(this, err));
        } else {
          this.onSocket(socket);
        }
      });

      try {
        const newSocket = opts.createConnection(opts, oncreate);
        if (newSocket) {
          oncreate(null, newSocket);
        }
      } catch (err) {
        oncreate(err);
      }
    } else {
      // this.onSocket(net.createConnection(opts));
    }
  }
}
Object.setPrototypeOf(ClientRequest.prototype, OutgoingMessage.prototype);
Object.setPrototypeOf(ClientRequest, OutgoingMessage);

ClientRequest.prototype._implicitHeader = function _implicitHeader() {
  if (this._header) {
    throw new ERR_HTTP_HEADERS_SENT('render');
  }
  this._storeHeader(
    this.method + ' ' + this.path + ' HTTP/1.1\r\n',
    this[kOutHeaders]
  );
};

ClientRequest.prototype.abort = function abort() {
  if (this.aborted) {
    return;
  }
  this.aborted = true;
  process.nextTick(emitAbortNT, this);
  this.destroy();
};

ClientRequest.prototype.destroy = function destroy(
  err?: Error | undefined | null
) {
  if (this.destroyed) {
    return this;
  }
  this.destroyed = true;

  // If we're aborting, we don't care about any more response data.
  if (this.res) {
    this.res._dump();
  }

  this[kError] = err;
  this.socket?.destroy(err);

  return this;
};

function emitAbortNT(req: ClientRequest): void {
  req.emit('abort');
}

function ondrain(this: any): void {
  const msg = this._httpMessage;
  if (msg && !msg.finished && msg[kNeedDrain]) {
    msg[kNeedDrain] = false;
    msg.emit('drain');
  }
}

function socketCloseListener(this: any): void {
  const socket = this;
  const req = socket._httpMessage;

  // NOTE: It's important to get parser here, because it could be freed by
  // the `socketOnData`.
  const parser = socket.parser;
  const res = req.res;

  req.destroyed = true;
  if (res) {
    // Socket closed before we emitted 'end' below.
    if (!res.complete) {
      res.destroy(new ConnResetException('aborted'));
    }
    req._closed = true;
    req.emit('close');
    if (!res.aborted && res.readable) {
      res.push(null);
    }
  } else {
    if (!req.socket._hadError) {
      // This socket error fired before we started to
      // receive a response. The error needs to
      // fire on the request.
      req.socket._hadError = true;
      emitErrorEvent(req, new ConnResetException('socket hang up'));
    }
    req._closed = true;
    req.emit('close');
  }

  // Too bad.  That output wasn't getting written.
  // This is pretty terrible that it doesn't raise an error.
  // Fixed better in v0.10
  if (req.outputData) req.outputData.length = 0;

  if (parser) {
    parser.finish();
  }
}

function socketErrorListener(this: any, err: any): void {
  const socket = this;
  const req = socket._httpMessage;

  if (req) {
    // For Safety. Some additional errors might fire later on
    // and we need to make sure we don't double-fire the error event.
    req.socket._hadError = true;
    emitErrorEvent(req, err);
  }

  const parser = socket.parser;
  if (parser) {
    parser.finish();
  }

  // Ensure that no further data will come out of the socket
  socket.removeListener('data', socketOnData);
  socket.removeListener('end', socketOnEnd);
  socket.destroy();
}

function socketOnEnd(this: any): void {
  const socket = this;
  const req = socket._httpMessage;
  const parser = socket.parser;

  if (!req.res && !req.socket._hadError) {
    // If we don't have a response then we know that the socket
    // ended prematurely and we need to emit an error on the request.
    req.socket._hadError = true;
    emitErrorEvent(req, new ConnResetException('socket hang up'));
  }
  if (parser) {
    parser.finish();
  }
  socket.destroy();
}

function socketOnData(this: any, d: Buffer): void {
  const socket = this;
  const req = this._httpMessage;
  const parser = this.parser;

  ok(parser && parser.socket === socket);

  const ret = parser.execute(d);
  if (ret instanceof Error) {
    socket.removeListener('data', socketOnData);
    socket.removeListener('end', socketOnEnd);
    socket.destroy();
    req.socket._hadError = true;
    emitErrorEvent(req, ret);
  } else if (parser.incoming?.upgrade) {
    // Upgrade (if status code 101) or CONNECT
    const bytesParsed = ret;
    const res = parser.incoming;
    req.res = res;

    socket.removeListener('data', socketOnData);
    socket.removeListener('end', socketOnEnd);
    socket.removeListener('drain', ondrain);

    if (req.timeoutCb) socket.removeListener('timeout', req.timeoutCb);
    socket.removeListener('timeout', responseOnTimeout);

    parser.finish();

    const bodyHead = d.slice(bytesParsed, d.length);

    const eventName = req.method === 'CONNECT' ? 'connect' : 'upgrade';
    if (req.listenerCount(eventName) > 0) {
      req.upgradeOrConnect = true;

      // detach the socket
      socket.emit('agentRemove');
      socket.removeListener('close', socketCloseListener);
      socket.removeListener('error', socketErrorListener);

      socket._httpMessage = null;
      socket.readableFlowing = null;

      req.emit(eventName, res, socket, bodyHead);
      req.destroyed = true;
      req._closed = true;
      req.emit('close');
    } else {
      // Requested Upgrade or used CONNECT method, but have no handler.
      socket.destroy();
    }
  } else if (
    parser.incoming?.complete &&
    // When the status code is informational (100, 102-199),
    // the server will send a final response after this client
    // sends a request body, so we must not free the parser.
    // 101 (Switching Protocols) and all other status codes
    // should be processed normally.
    !statusIsInformational(parser.incoming.statusCode)
  ) {
    socket.removeListener('data', socketOnData);
    socket.removeListener('end', socketOnEnd);
    socket.removeListener('drain', ondrain);
  }
}

function statusIsInformational(status: number): boolean {
  // 100 (Continue)    RFC7231 Section 6.2.1
  // 102 (Processing)  RFC2518
  // 103 (Early Hints) RFC8297
  // 104-199 (Unassigned)
  return status < 200 && status >= 100 && status !== 101;
}

function responseOnTimeout(this: ClientRequest): void {
  const req = this._httpMessage;
  if (!req) return;
  const res = req.res;
  if (!res) return;
  res.emit('timeout');
}

function tickOnSocket(req: ClientRequest, socket: any): void {
  // const parser = parsers.alloc();
  // req.socket = socket;
  // const lenient = false;
  // parser.initialize(
  //   HTTPParser.RESPONSE,
  //   new HTTPClientAsyncResource('HTTPINCOMINGMESSAGE', req),
  //   req.maxHeaderSize || 0,
  //   lenient ? kLenientAll : kLenientNone
  // );
  // parser.socket = socket;
  // parser.outgoing = req;
  // req.parser = parser;

  // socket.parser = parser;
  // socket._httpMessage = req;

  // // Propagate headers limit from request object to parser
  // if (typeof req.maxHeadersCount === 'number') {
  //   parser.maxHeaderPairs = req.maxHeadersCount << 1;
  // }

  // parser.joinDuplicateHeaders = req.joinDuplicateHeaders;

  // parser.onIncoming = parserOnIncomingClient;
  socket.on('error', socketErrorListener);
  socket.on('data', socketOnData);
  socket.on('end', socketOnEnd);
  socket.on('close', socketCloseListener);
  socket.on('drain', ondrain);

  if (req.timeout !== undefined || req.agent?.options?.timeout) {
    listenSocketTimeout(req);
  }
  req.emit('socket', socket);
}

function emitRequestTimeout(this: ClientRequest): void {
  const req = this._httpMessage;
  if (req) {
    req.emit('timeout');
  }
}

function listenSocketTimeout(req: ClientRequest): void {
  if (req.timeoutCb) {
    return;
  }
  // Set timeoutCb so it will get cleaned up on request end.
  req.timeoutCb = emitRequestTimeout;
  // Delegate socket timeout event.
  if (req.socket) {
    req.socket.once('timeout', emitRequestTimeout);
  } else {
    req.on('socket', (socket) => {
      socket.once('timeout', emitRequestTimeout);
    });
  }
}

ClientRequest.prototype.onSocket = function onSocket(
  socket: unknown,
  err?: Error
): void {
  // TODO(ronag): Between here and onSocketNT the socket
  // has no 'error' handler.
  process.nextTick(onSocketNT, this, socket, err);
};

function onSocketNT(req: ClientRequest, socket: any, err?: Error): void {
  if (req.destroyed || err) {
    req.destroyed = true;

    function _destroy(req: ClientRequest, err?: Error | null): void {
      if (!req.aborted && !err) {
        err = new ConnResetException('socket hang up');
      }
      if (err) {
        emitErrorEvent(req, err);
      }
      req._closed = true;
      req.emit('close');
    }

    if (socket) {
      if (!err && req.agent && !socket.destroyed) {
        socket.emit('free');
      } else {
        finished(socket.destroy(err || req[kError]), (er) => {
          if (er?.code === 'ERR_STREAM_PREMATURE_CLOSE') {
            er = null;
          }
          _destroy(req, er || err);
        });
        return;
      }
    }

    _destroy(req, err || req[kError]);
  } else {
    tickOnSocket(req, socket);
    req._flush();
  }
}

ClientRequest.prototype._deferToConnect = _deferToConnect;
function _deferToConnect(
  this: ClientRequest,
  method: string,
  arguments_: unknown[]
): void {
  // This function is for calls that need to happen once the socket is
  // assigned to this request and writable. It's an important promisy
  // thing for all the socket calls that happen either now
  // (when a socket is assigned) or in the future (when a socket gets
  // assigned out of the pool and is eventually writable).

  const callSocketMethod = (): void => {
    if (method) {
      Reflect.apply(this.socket[method], this.socket, arguments_);
    }
  };

  const onSocket = (): void => {
    if (this.socket.writable) {
      callSocketMethod();
    } else {
      this.socket.once('connect', callSocketMethod);
    }
  };

  if (!this.socket) {
    this.once('socket', onSocket);
  } else {
    onSocket();
  }
}

ClientRequest.prototype.setTimeout = function setTimeout(
  msecs: number,
  callback?: VoidFunction
): ClientRequest {
  if (this._ended) {
    return this;
  }

  listenSocketTimeout(this);
  msecs = getTimerDuration(msecs, 'msecs');
  if (callback) this.once('timeout', callback);

  if (this.socket) {
    setSocketTimeout(this.socket, msecs);
  } else {
    this.once('socket', (sock) => setSocketTimeout(sock, msecs));
  }

  return this;
};

function setSocketTimeout(sock: any, msecs: number): void {
  if (sock.connecting) {
    sock.once('connect', function () {
      sock.setTimeout(msecs);
    });
  } else {
    sock.setTimeout(msecs);
  }
}

ClientRequest.prototype.setNoDelay = function setNoDelay(
  noDelay?: boolean
): void {
  this._deferToConnect('setNoDelay', [noDelay]);
};

ClientRequest.prototype.setSocketKeepAlive = function setSocketKeepAlive(
  enable?: boolean,
  initialDelay?: number
): void {
  this._deferToConnect('setKeepAlive', [enable, initialDelay]);
};

ClientRequest.prototype.clearTimeout = function clearTimeout(
  cb?: VoidFunction
): void {
  this.setTimeout(0, cb);
};
