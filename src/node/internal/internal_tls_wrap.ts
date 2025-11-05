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

import {
  Socket,
  type SocketOptions,
  _normalizeArgs,
  onConnectionOpened,
  onConnectionClosed,
  tryReadStart,
} from 'node-internal:internal_net';
import { JSStreamSocket } from 'node-internal:internal_tls_jsstream';
import { checkServerIdentity } from 'node-internal:internal_tls';
import type {
  ConnectionOptions,
  TlsOptions,
  TLSSocket as TLSSocketType,
} from 'node:tls';
import type { Duplex } from 'node-internal:streams_duplex';
import type { OnReadOpts, TcpSocketConnectOpts } from 'node:net';
import {
  validateBuffer,
  validateString,
  validateObject,
  validateNumber,
  validateFunction,
  validateInt32,
  validateUint32,
} from 'node-internal:validators';
import {
  ConnResetException,
  ERR_TLS_HANDSHAKE_TIMEOUT,
  ERR_OPTION_NOT_IMPLEMENTED,
  ERR_TLS_INVALID_CONTEXT,
} from 'node-internal:internal_errors';
import { SecureContext } from 'node-internal:internal_tls_common';
import { ok } from 'node-internal:internal_assert';

const kConnectOptions = Symbol('connect-options');
const kErrorEmitted = Symbol('error-emitted');
const kRes = Symbol('res');
const kPendingSession = Symbol('pendingSession');
const kIsVerified = Symbol('verified');

// @ts-expect-error TS2323 Cannot redeclare error.
export declare class TLSSocket extends Socket {
  _hadError: boolean;
  _handle: Socket['_handle'];
  _init(): void;
  _tlsOptions: TlsOptions &
    ConnectionOptions &
    SocketOptions &
    TcpSocketConnectOpts & {
      isServer?: boolean;
      requestOCSP?: boolean;
      server?: unknown;
      onread?: OnReadOpts;
    };
  _secureEstablished: boolean;
  _securePending: boolean;
  _newSessionPending: boolean;
  _controlReleased: boolean;
  authorized: boolean;
  encrypted: boolean;
  handle: ReturnType<TLSSocket['_wrapHandle']>;
  servername: null | string;
  secureConnecting: boolean;
  ssl: TLSSocket['_handle'];
  [kRes]: null | Socket['_handle'];
  [kIsVerified]: boolean;
  [kPendingSession]: null | Buffer;
  [kErrorEmitted]: boolean;
  [kConnectOptions]?: NormalizedConnectionOptions;

  constructor(
    socket: Socket | Duplex | undefined,
    opts: TLSSocket['_tlsOptions']
  );
  prototype: TLSSocket;

  _destroySSL(): void;
  _emitTLSError(error: Error): void;
  _finishInit(): void;
  _handleTimeout(): void;
  _releaseControl(): boolean;
  _start(): void;
  _tlsError(error: Error): Error | null;
  _wrapHandle(
    wrap: null | Socket,
    handle: Socket['_handle'] | null | undefined,
    wrapHasActiveWriteFromPrevOwner: boolean
  ): unknown;
  disableRenegotiation(): void;
  getX509Certificate(): ReturnType<TLSSocketType['getX509Certificate']>;
  setKeyCert(context: unknown): void;
  setServername(name: string): void;
  setSession(session: string | Buffer): void;
  setMaxSendFragment(size: number): boolean;
  getCertificate(): ReturnType<TLSSocketType['getCertificate']>;
  getPeerX509Certificate(): ReturnType<TLSSocketType['getPeerX509Certificate']>;
  renegotiate(
    options: {
      rejectUnauthorized?: boolean | undefined;
      requestCert?: boolean | undefined;
    },
    callback?: (error: Error | null) => void
  ): boolean;
  exportKeyingMaterial(length: number, label: string, context?: Buffer): Buffer;
}

function onnewsessionclient(
  this: TLSSocket,
  _sessionId: string,
  session: Buffer
): void {
  if (this[kIsVerified]) {
    this.emit('session', session);
  } else {
    this[kPendingSession] = session;
  }
}

function onerror(this: TLSSocket, err: Error): void {
  if (this._hadError) return;

  this._hadError = true;

  // Destroy socket if error happened before handshake's finish
  if (!this._secureEstablished) {
    // When handshake fails control is not yet released,
    // so self._tlsError will return null instead of actual error

    // Set closing the socket after emitting an event since the socket needs to
    // be accessible when the `tlsClientError` event is emitted.
    this.destroy(err);
  } else {
    // Emit error
    this._emitTLSError(err);
  }
}

// We are using old style function classes for node.js compat.
// @ts-expect-error TS2323 Cannot redeclare error.
export function TLSSocket(
  this: TLSSocket,
  socket: Socket | Duplex | undefined,
  opts: TLSSocket['_tlsOptions']
): void {
  const tlsOptions = { ...opts };

  if (tlsOptions.enableTrace) {
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.enableTrace');
  }

  if (tlsOptions.isServer) {
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.isServer');
  }

  if (tlsOptions.server) {
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.server');
  }

  if (tlsOptions.requestCert) {
    // Servers will request certificate from clients.
    // Does not apply to Cloudflare Workers.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.requestCert');
  }

  if (tlsOptions.rejectUnauthorized === false) {
    // TODO(soon): We don't support rejectUnauthorized=false
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.rejectUnauthorized');
  }

  if (tlsOptions.ALPNProtocols !== undefined) {
    // Does not apply to Cloudflare Workers.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.ALPNProtocols');
  }

  if (tlsOptions.SNICallback !== undefined) {
    // Does not apply to Cloudflare Workers.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.SNICallback');
  }

  if (tlsOptions.requestOCSP) {
    // Not yet supported. Can be implemented in the future.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.requestOCSP');
  }

  if (
    tlsOptions.secureContext !== undefined &&
    !(tlsOptions.secureContext instanceof SecureContext)
  ) {
    throw new ERR_TLS_INVALID_CONTEXT('context');
  }

  if (tlsOptions.pskCallback !== undefined) {
    // Used for TLS-PSK negotiation. We do not support it.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.pskCallback');
  }

  // TODO(soon): Call this on secureConnect once connect() api supports
  // getting peer certificate.
  if (tlsOptions.checkServerIdentity !== undefined) {
    validateFunction(
      tlsOptions.checkServerIdentity,
      'options.checkServerIdentity'
    );
  }

  this._tlsOptions = tlsOptions;
  this._secureEstablished = false;
  this._securePending = false;
  this._newSessionPending = false;
  this._controlReleased = false;
  this.secureConnecting = true;
  this.servername = null;
  this.authorized = false;
  this[kRes] = null;
  this[kIsVerified] = false;
  this[kPendingSession] = null;
  this[kErrorEmitted] = false;

  let wrap: Socket | null = null;
  let handle: Socket['_handle'] | null = null;
  let wrapHasActiveWriteFromPrevOwner = false;

  if (socket) {
    if (socket instanceof Socket) {
      wrap = socket;
    } else {
      wrap = new JSStreamSocket(socket);
    }

    handle = wrap._handle;
    wrapHasActiveWriteFromPrevOwner = wrap.writableLength > 0;
  }

  // Just a documented property to make secure sockets
  // distinguishable from regular ones.
  this.encrypted = true;

  Reflect.apply(Socket, this, [
    {
      handle: this._wrapHandle(wrap, handle, wrapHasActiveWriteFromPrevOwner),
      allowHalfOpen: socket ? socket.allowHalfOpen : tlsOptions.allowHalfOpen,
      pauseOnCreate: tlsOptions.pauseOnConnect,
      manualStart: true,
      highWaterMark: tlsOptions.highWaterMark,
      onread: !socket ? tlsOptions.onread : null,
      signal: tlsOptions.signal,
      lookup: tlsOptions.lookup,
    } as SocketOptions,
  ]);

  this._parent = handle;
  this._parentWrap = wrap;

  // Proxy for API compatibility
  this.ssl = this._handle; // C++ TLSWrap object

  this.on('error', this._tlsError.bind(this));

  this._init();
}
Object.setPrototypeOf(TLSSocket.prototype, Socket.prototype);
Object.setPrototypeOf(TLSSocket, Socket);

TLSSocket.prototype.disableRenegotiation = function disableRenegotiation(
  this: TLSSocket
): void {
  // Do nothing.
};

TLSSocket.prototype._wrapHandle = function _wrapHandle(
  this: TLSSocket,
  wrap: null | Socket,
  handle: Socket['_handle'] | null | undefined,
  _wrapHasActiveWriteFromPrevOwner: boolean
): unknown {
  if (!handle) {
    return null;
  }
  this[kRes] = handle;

  // Guard against adding multiple listeners, as this method may be called
  // repeatedly on the same socket by reinitializeHandle
  if (this.listenerCount('close', onSocketCloseDestroySSL) === 0) {
    this.once('close', onSocketCloseDestroySSL);
  }

  wrap?.once('close', () => this.destroy());

  return handle;
};

function onSocketCloseDestroySSL(this: TLSSocket): void {
  // We call destroySSL directly because it is already an async process.
  this._destroySSL();
  this[kRes] = null;
}

TLSSocket.prototype._destroySSL = function _destroySSL(this: TLSSocket): void {
  // We disable floating promises rule because we don't want to change the
  // function signature and return Promise<void>
  //
  // eslint-disable-next-line @typescript-eslint/no-floating-promises
  this._handle?.socket.close().then(() => {
    this[kPendingSession] = null;
    this[kIsVerified] = false;
  });
};

TLSSocket.prototype._init = function _init(this: TLSSocket): void {
  const options = this._tlsOptions;

  this.on('error', onerror.bind(this));

  // This is emitted from net.Socket class.
  this.on('connectionAttempt', onnewsessionclient.bind(this));

  if (options.handshakeTimeout && options.handshakeTimeout > 0)
    this.setTimeout(options.handshakeTimeout, this._handleTimeout.bind(this));

  // TLSSocket can be initialized with 2 different handles.
  //
  // 1. Socket instance created by "node:net". In this scenario, we need
  //    to wait for 'connect' event to be emitted in order to trigger _finishInit().
  // 2. Duplex stream. Duplex streams are initialized through JSStreamSocket class.
  //    If that's the scenario, we can trigger _finishInit() immediately. Since, there
  //    is no async calls required to wait.
  if (this._parentWrap != null && this._parentWrap instanceof JSStreamSocket) {
    queueMicrotask(() => {
      this._finishInit();
      ok(this._parentWrap instanceof JSStreamSocket);
      this._parentWrap.readStart();
      tryReadStart(this);
    });
  } else {
    this.on('connect', () => {
      this._finishInit();
    });
  }
};

TLSSocket.prototype.renegotiate = function (
  this: TLSSocket,
  options: {
    rejectUnauthorized?: boolean | undefined;
    requestCert?: boolean | undefined;
  },
  callback?: (error: Error | null) => void
): boolean {
  validateObject(options, 'options');
  if (callback !== undefined) {
    validateFunction(callback, 'callback');
  }
  // TLS renegotiation is not supported.
  return false;
};

TLSSocket.prototype.exportKeyingMaterial = function exportKeyingMaterial(
  this: TLSSocket,
  length: number,
  label: string,
  context?: Buffer
): Buffer {
  validateUint32(length, 'length', true);
  validateString(label, 'label');
  if (context !== undefined) {
    validateBuffer(context, 'context');
  }

  throw new Error('exportKeyingMaterial is not implemented');
};

TLSSocket.prototype.setMaxSendFragment = function setMaxSendFragment(
  this: TLSSocket,
  size: number
): boolean {
  validateInt32(size, 'size');
  // Setting maximum TLS fragment size is not supported.
  return false;
};

TLSSocket.prototype._handleTimeout = function _handleTimeout(
  this: TLSSocket
): void {
  this._emitTLSError(new ERR_TLS_HANDSHAKE_TIMEOUT());
};

TLSSocket.prototype._emitTLSError = function _emitTLSError(
  this: TLSSocket,
  err: Error
): void {
  const e = this._tlsError(err);
  if (e) this.emit('error', e);
};

TLSSocket.prototype._tlsError = function _tlsError(
  this: TLSSocket,
  err: Error
): Error | null {
  this.emit('_tlsError', err);
  if (this._controlReleased) return err;
  return null;
};

TLSSocket.prototype._releaseControl = function _releaseControl(
  this: TLSSocket
): boolean {
  if (this._controlReleased) return false;
  this._controlReleased = true;
  this.removeListener('error', this._tlsError.bind(this));
  return true;
};

// This function is called from net.Socket onConnectionOpened() handler.
TLSSocket.prototype._finishInit = function _finishInit(this: TLSSocket): void {
  // Guard against getting onhandshakedone() after .destroy().
  // * 1.2: If destroy() during onocspresponse(), then write of next handshake
  // record fails, the handshake done info callbacks does not occur, and the
  // socket closes.
  // * 1.3: The OCSP response comes in the same record that finishes handshake,
  // so even after .destroy(), the handshake done info callback occurs
  // immediately after onocspresponse(). Ignore it.
  if (!this._handle) return;

  this._secureEstablished = true;
  if (
    this._tlsOptions.handshakeTimeout &&
    this._tlsOptions.handshakeTimeout > 0
  ) {
    this.setTimeout(0, this._handleTimeout.bind(this));
  }

  this.emit('secure');
};

TLSSocket.prototype._start = function _start(this: TLSSocket): void {
  if (this.connecting) {
    this.once('connect', this._start.bind(this));
    return;
  }

  // If a user calls tls.connect({ socket }) with a socket that is not initialized
  // and the socket is not connected, we need to wait for the socket to connect
  // before we can complete the process.
  //
  // Take a look at the following test for this particular edge case:
  // https://github.com/nodejs/node/blob/91d8a524ada001103a2d1c6825ca17b8393c183f/test/parallel/test-tls-on-empty-socket.js
  if (this._parentWrap != null && this._parentWrap._handle == null) {
    this._parentWrap.once('connect', () => {
      // We need to update the Socket handle of this TLSSocket
      // since it was created after TLSSocket is initialized.
      if (this._parentWrap?._handle != null) {
        this._handle = this._parentWrap._handle;
      }
      this._start();
    });
    return;
  }

  // Guard against the following cases:
  // - Socket was destroyed before the connection was established
  // - TLSSocket can not be upgraded if the secureTransport does not support 'starttls'
  if (this._handle?.socket.secureTransport !== 'starttls') {
    return;
  }

  // We first need to release the lock
  this._handle.writer.releaseLock();
  this._handle.reader.releaseLock();

  try {
    const { host, port, addressType } = this._handle.options;
    const socket = this._handle.socket.startTls();

    this._handle = {
      socket: socket,
      writer: socket.writable.getWriter(),
      reader: socket.readable.getReader({ mode: 'byob' }),
      bytesRead: 0,
      bytesWritten: 0,
      reading: true,
      options: this._handle.options,
    };

    // This is now an encrypted connection.
    // There are cases where in node:net we have to distinguish between
    // encrypted and unencrypted connections.
    this.encrypted = true;

    this._handle.socket.opened.then(
      onConnectionOpened.bind(this),
      (err: unknown) => {
        this.emit('connectionAttemptFailed', host, port, addressType, err);
        this.destroy(err as Error);
      }
    );

    this._handle.socket.closed.then(
      onConnectionClosed.bind(this),
      (error: unknown): void => {
        // Do not call this.destroy.bind(this) since user can override it.
        this.destroy(error as Error);
      }
    );
  } catch (error) {
    this.destroy(error as Error);
  }
};

TLSSocket.prototype.setServername = function setServername(
  this: TLSSocket,
  name: string
): void {
  validateString(name, 'name');
  // Pipefitter currently does not provide us a way on the internal
  // system and possibly KJ's TLS implementation doesn't provides a way,
  // but it is something we will need sooner than later.
};

TLSSocket.prototype.setSession = function (_session: string | Buffer): void {
  // Do nothing. We don't support setting session.
};

// @ts-expect-error TS2322 Inconsistencies between @types/node
TLSSocket.prototype.getPeerCertificate = function (
  _detailed?: boolean
): ReturnType<TLSSocketType['getPeerCertificate']> {
  // Returns an object representing a peer certificate.
  // This function is not supported.
  throw new Error('getPeerCertificate is not implemented');
};

TLSSocket.prototype.getCertificate = function (
  this: TLSSocket
): ReturnType<TLSSocketType['getCertificate']> {
  // Returns an object representing the local certificate.
  // This function is not supported.
  throw new Error('TLSSocket.getCertificate is not implemented');
};

TLSSocket.prototype.getPeerX509Certificate = function (
  this: TLSSocket,
  _detailed?: boolean
): ReturnType<TLSSocketType['getPeerX509Certificate']> {
  // Returns the peer certificate as an X509 certificate.
  // This function is not supported.
  throw new Error('TLSSocket.getPeerX509Certificate is not implemented');
};

TLSSocket.prototype.getX509Certificate = function (
  this: TLSSocket
): ReturnType<TLSSocketType['getX509Certificate']> {
  // Returns the local certificate as an X509 certificate.
  // This function is not supported.
  throw new Error('TLSSocket.getX509Certificate is not implemented');
};

TLSSocket.prototype.setKeyCert = function (
  this: TLSSocket,
  _context: unknown
): void {
  // Changing private key and certificate to be used with TCP connection
  // is not supported due to the limitations of connect() api.
  throw new Error('TLSSocket.setKeyCert is not implemented');
};

// We have this syntax because of the original Node.js implementation.
// They are not supported by Cloudflare Workers but in Node.js
// they are all properties of this._handle[PROP_NAME]
[
  'getCipher',
  'getSharedSigalgs',
  'getEphemeralKeyInfo',
  'getFinished',
  'getPeerFinished',
  'getProtocol',
  'getSession',
  'getTLSTicket',
  'isSessionReused',
  'enableTrace',
].forEach((method) => {
  // @ts-expect-error TS7053 Omitting...
  TLSSocket.prototype[method] = function (): null {
    // None of these functions are supported by connect() api.
    return null;
  };
});

type NormalizedConnectionOptions = ConnectionOptions &
  SocketOptions &
  TlsOptions & {
    host?: string;
    port: number;
  };
function normalizeConnectArgs(
  listArgs: unknown[]
): [NormalizedConnectionOptions] | [NormalizedConnectionOptions, VoidFunction] {
  const args = _normalizeArgs(listArgs);
  const options = args[0] as NormalizedConnectionOptions;
  const cb = args[1] as VoidFunction | undefined;

  // If args[0] was options, then normalize dealt with it.
  // If args[0] is port, or args[0], args[1] is host, port, we need to
  // find the options and merge them in, normalize's options has only
  // the host/port/path args that it knows about, not the tls options.
  // This means that options.host overrides a host arg.
  if (listArgs[1] !== null && typeof listArgs[1] === 'object') {
    Object.assign(options, listArgs[1]);
  } else if (listArgs[2] !== null && typeof listArgs[2] === 'object') {
    Object.assign(options, listArgs[2]);
  }

  return cb ? [options, cb] : [options];
}

function onConnectSecure(this: TLSSocket): void {
  this.authorized = true;
  this.secureConnecting = false;
  this.emit('secureConnect');

  this[kIsVerified] = true;
  const session = this[kPendingSession];
  this[kPendingSession] = null;
  if (session) this.emit('session', session);

  this.removeListener('end', onConnectEnd);
}

function onConnectEnd(this: TLSSocket): void {
  // NOTE: This logic is shared with _http_client.js
  if (!this._hadError) {
    const options = this[kConnectOptions];
    this._hadError = true;
    const error = new ConnResetException(
      'Client network socket disconnected ' +
        'before secure TLS connection was ' +
        'established'
    );
    error.path = options?.path;
    error.host = options?.host;
    error.port = options?.port;
    // @ts-expect-error TS2339 Missing types
    // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
    error.localAddress = options?.localAddress;
    this.destroy(error);
  }
}

// Arguments: [port,] [host,] [options,] [cb]
export function connect(...args: unknown[]): TLSSocket {
  args = normalizeConnectArgs(args);
  const options = args[0] as NormalizedConnectionOptions;
  const cb = args[1] as VoidFunction | undefined;

  if (options.minDHSize !== undefined) {
    // We leave this validation for node.js compat.
    validateNumber(options.minDHSize, 'options.minDHSize', 1);
    // Not supported.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.minDHSize');
  }

  if (options.rejectUnauthorized === false) {
    // TODO(soon): We don't support rejectUnauthorized=false
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.rejectUnauthorized');
  }

  if (options.pskCallback !== undefined) {
    // Used for TLS-PSK negotiation.
    // Does not make sense for Cloudflare Workers.
    throw new ERR_OPTION_NOT_IMPLEMENTED('options.pskCallback');
  }

  if (options.checkServerIdentity !== undefined) {
    validateFunction(
      options.checkServerIdentity,
      'options.checkServerIdentity'
    );
  }

  // @ts-expect-error TS2345 Type incompatibility between Node.js Duplex and internal Duplex
  const tlssock = new TLSSocket(options.socket, {
    allowHalfOpen: options.allowHalfOpen,
    pipe: !!options.path,
    ALPNProtocols: options.ALPNProtocols,
    enableTrace: options.enableTrace,
    highWaterMark: options.highWaterMark,
    secureContext: options.secureContext,
    checkServerIdentity: options.checkServerIdentity ?? checkServerIdentity,
    onread: options.onread,
    signal: options.signal,
    lookup: options.lookup,
    rejectUnauthorized:
      options.rejectUnauthorized !== undefined
        ? Boolean(options.rejectUnauthorized) // eslint-disable-line @typescript-eslint/no-unnecessary-type-conversion
        : true,
  });

  tlssock[kConnectOptions] = options;

  if (cb) {
    tlssock.once('secureConnect', cb);
  }

  if (!options.socket) {
    // If user provided the socket, it's their responsibility to manage its
    // connectivity. If we created one internally, we connect it.
    if (options.timeout) {
      tlssock.setTimeout(options.timeout);
    }

    tlssock.connect(options, tlssock._start.bind(tlssock));
  }

  tlssock._releaseControl();

  if (options.session) {
    tlssock.setSession(options.session);
  }

  if (options.socket) {
    tlssock._start();
  }

  // The 'secure' event is emitted by the SecurePair object once a secure connection has been established.
  tlssock.on('secure', onConnectSecure);
  tlssock.prependListener('end', onConnectEnd);

  return tlssock;
}
