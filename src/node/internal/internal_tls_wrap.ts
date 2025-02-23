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
  normalizeArgs as _normalizeArgs,
  kReinitializeHandle,
} from 'node-internal:internal_net';
import type {
  ConnectionOptions,
  TlsOptions,
  TLSSocket as TLSSocketType,
} from 'node:tls';
import { ok as assert } from 'node-internal:internal_assert';
import {
  validateBuffer,
  validateString,
  validateObject,
  validateNumber,
  validateFunction,
} from 'node-internal:validators';
import { isArrayBufferView } from 'node-internal:internal_types';
import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_SOCKET_CLOSED,
} from 'node-internal:internal_errors';

const kConnectOptions = Symbol('connect-options');
const kDisableRenegotiation = Symbol('disable-renegotiation');
const kErrorEmitted = Symbol('error-emitted');
const kHandshakeTimeout = Symbol('handshake-timeout');
const kRes = Symbol('res');
const kSNICallback = Symbol('snicallback');
const kALPNCallback = Symbol('alpncallback');
const kEnableTrace = Symbol('enableTrace');
const kPskCallback = Symbol('pskcallback');
const kPskIdentityHint = Symbol('pskidentityhint');
const kPendingSession = Symbol('pendingSession');
const kIsVerified = Symbol('verified');

export type TLSSocketClass = TLSSocketType & {
  _handle: {
    certCbDone(): void;
    enableTrace(): void;
    enableKeylogCallback(): void;
    isStreamBase: boolean;
  };
  _tlsOptions: TlsOptions;
  _secureEstablished: boolean;
  _securePending: boolean;
  _newSessionPending: boolean;
  _controlReleased: boolean;
  _SNICallback: null;
  authorizationError: null | Error;
  servername: null | string;
  secureConnecting: boolean;
  [kRes]: null;
  [kIsVerified]: boolean;
  [kPendingSession]: null;
  [kErrorEmitted]: boolean;
  [kDisableRenegotiation]: boolean;
  [kPskCallback]?: TlsOptions['pskCallback'];
  [kALPNCallback]: VoidFunction | null;

  new (): TLSSocketClass;
  prototype: TLSSocketClass;

  _destroySSL(): void;
  disableRenegotiation(): void;
  setServername(name: string): void;
};

function requestOCSP(socket, info) {
  if (!info.OCSPRequest || !socket.server) return requestOCSPDone(socket);

  let ctx = socket._handle.sni_context;

  if (!ctx) {
    ctx = socket.server._sharedCreds;

    // TLS socket is using a `net.Server` instead of a tls.TLSServer.
    // Some TLS properties like `server._sharedCreds` will not be present
    if (!ctx) return requestOCSPDone(socket);
  }

  // TODO(indutny): eventually disallow raw `SecureContext`
  if (ctx.context) ctx = ctx.context;

  if (socket.server.listenerCount('OCSPRequest') === 0) {
    return requestOCSPDone(socket);
  }

  let once = false;
  const onOCSP = (err, response) => {
    if (once) return socket.destroy(new ERR_MULTIPLE_CALLBACK());
    once = true;

    if (err) return socket.destroy(err);

    if (socket._handle === null) return socket.destroy(new ERR_SOCKET_CLOSED());

    if (response) socket._handle.setOCSPResponse(response);
    requestOCSPDone(socket);
  };

  socket.server.emit(
    'OCSPRequest',
    ctx.getCertificate(),
    ctx.getIssuer(),
    onOCSP
  );
}

function requestOCSPDone(socket: TLSSocketClass) {
  try {
    socket._handle.certCbDone();
  } catch (e) {
    socket.destroy(e as Error);
  }
}

function onnewsessionclient(_sessionId: string, session: Buffer) {
  const owner = this[owner_symbol];
  if (owner[kIsVerified]) {
    owner.emit('session', session);
  } else {
    owner[kPendingSession] = session;
  }
}

function onPskServerCallback(identity, maxPskLen) {
  const owner = this[owner_symbol];
  const ret = owner[kPskCallback](owner, identity);
  if (ret == null) return undefined;

  let psk;
  if (isArrayBufferView(ret)) {
    psk = ret;
  } else {
    if (typeof ret !== 'object') {
      throw new ERR_INVALID_ARG_TYPE(
        'ret',
        ['Object', 'Buffer', 'TypedArray', 'DataView'],
        ret
      );
    }
    psk = ret.psk;
    validateBuffer(psk, 'psk');
  }

  if (psk.length > maxPskLen) {
    throw new ERR_INVALID_ARG_VALUE(
      'psk',
      psk,
      `Pre-shared key exceeds ${maxPskLen} bytes`
    );
  }

  return psk;
}

function onPskClientCallback(hint, maxPskLen, maxIdentityLen) {
  const owner = this[owner_symbol];
  const ret = owner[kPskCallback](hint);
  if (ret == null) return undefined;

  validateObject(ret, 'ret');

  validateBuffer(ret.psk, 'psk');
  if (ret.psk.length > maxPskLen) {
    throw new ERR_INVALID_ARG_VALUE(
      'psk',
      ret.psk,
      `Pre-shared key exceeds ${maxPskLen} bytes`
    );
  }

  validateString(ret.identity, 'identity');
  if (Buffer.byteLength(ret.identity) > maxIdentityLen) {
    throw new ERR_INVALID_ARG_VALUE(
      'identity',
      ret.identity,
      `PSK identity exceeds ${maxIdentityLen} bytes`
    );
  }

  return { psk: ret.psk, identity: ret.identity };
}

function onkeylog(line: unknown): void {
  this[owner_symbol].emit('keylog', line);
}

function onocspresponse(resp: unknown): void {
  this[owner_symbol].emit('OCSPResponse', resp);
}

function onerror(err: Error): void {
  const owner = this[owner_symbol];

  if (owner._hadError) return;

  owner._hadError = true;

  // Destroy socket if error happened before handshake's finish
  if (!owner._secureEstablished) {
    // When handshake fails control is not yet released,
    // so self._tlsError will return null instead of actual error

    // Set closing the socket after emitting an event since the socket needs to
    // be accessible when the `tlsClientError` event is emitted.
    owner._closeAfterHandlingError = true;
    owner.destroy(err);
  } else if (
    owner._tlsOptions?.isServer &&
    owner._rejectUnauthorized &&
    /peer did not return a certificate/.test(err.message)
  ) {
    // Ignore server's authorization errors
    owner.destroy();
  } else {
    // Emit error
    owner._emitTLSError(err);
  }
}

// Used by both client and server TLSSockets to start data flowing from _handle,
// read(0) causes a StreamBase::ReadStart, via Socket._read.
function initRead(tlsSocket: TLSSocketClass, socket: typeof Socket): void {
  // If we were destroyed already don't bother reading
  if (!tlsSocket._handle) return;

  // Socket already has some buffered data - emulate receiving it
  if (socket?.readableLength) {
    let buf;
    while ((buf = socket.read()) !== null) tlsSocket._handle.receive(buf);
  }

  tlsSocket.read(0);
}

/**
 * Provides a wrap of socket stream to do encrypted communication.
 */

export const TLSSocket = function TLSSocket(
  this: TLSSocketClass,
  socket: typeof Socket,
  opts: TlsOptions
) {
  const tlsOptions = { ...opts };

  if (tlsOptions.ALPNProtocols)
    tls.convertALPNProtocols(tlsOptions.ALPNProtocols, tlsOptions);

  this._tlsOptions = tlsOptions;
  this._secureEstablished = false;
  this._securePending = false;
  this._newSessionPending = false;
  this._controlReleased = false;
  this.secureConnecting = true;
  this._SNICallback = null;
  this[kALPNCallback] = null;
  this.servername = null;
  this.alpnProtocol = null;
  this.authorized = false;
  this.authorizationError = null;
  this[kRes] = null;
  this[kIsVerified] = false;
  this[kPendingSession] = null;
  this[kErrorEmitted] = false;

  let wrap;
  let handle;
  let wrapHasActiveWriteFromPrevOwner;

  if (socket) {
    if (socket instanceof Socket && socket._handle) {
      // 1. connected socket
      wrap = socket;
    } else {
      // 2. socket has no handle so it is js not c++
      // 3. unconnected sockets are wrapped
      // TLS expects to interact from C++ with a Socket that has a C++ stream
      // handle, but a JS stream doesn't have one. Wrap it up to make it look like
      // a socket.
      wrap = new JSStreamSocket(socket);
    }

    handle = wrap._handle;
    wrapHasActiveWriteFromPrevOwner = wrap.writableLength > 0;
  } else {
    // 4. no socket, one will be created with Socket().connect
    wrap = null;
    wrapHasActiveWriteFromPrevOwner = false;
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
    },
  ]);

  // Proxy for API compatibility
  this.ssl = this._handle; // C++ TLSWrap object

  this.on('error', this._tlsError);

  this._init(socket, wrap);

  if (wrapHasActiveWriteFromPrevOwner) {
    // `wrap` is a streams.Writable in JS. This empty write will be queued
    // and hence finish after all existing writes, which is the timing
    // we want to start to send any tls data to `wrap`.
    wrap.write('', (err: Error | null) => {
      if (err) {
        this.destroy(err);
        return;
      }

      this._handle.writesIssuedByPrevListenerDone();
    });
  }

  // Read on next tick so the caller has a chance to setup listeners
  process.nextTick(initRead, this, socket);
};
Object.setPrototypeOf(TLSSocket.prototype, Socket.prototype);
Object.setPrototypeOf(TLSSocket, Socket);

TLSSocket.prototype.disableRenegotiation =
  function disableRenegotiation(): void {
    this[kDisableRenegotiation] = true;
  };

TLSSocket.prototype._wrapHandle = function _wrapHandle(
  this: TLSSocketClass,
  wrap: null | typeof Socket,
  handle: null,
  wrapHasActiveWriteFromPrevOwner: boolean
) {
  const options = this._tlsOptions;
  if (!handle) {
    handle = options.pipe
      ? new Pipe(PipeConstants.SOCKET)
      : new TCP(TCPConstants.SOCKET);
    handle[owner_symbol] = this;
  }

  // Wrap socket's handle
  const context =
    options.secureContext ||
    options.credentials ||
    tls.createSecureContext(options);
  assert(handle.isStreamBase, 'handle must be a StreamBase');
  if (!(context.context instanceof NativeSecureContext)) {
    throw new ERR_TLS_INVALID_CONTEXT('context');
  }

  const res = tls_wrap.wrap(
    handle,
    context.context,
    !!options.isServer,
    wrapHasActiveWriteFromPrevOwner
  );
  res._parent = handle; // C++ "wrap" object: TCPWrap, JSStream, ...
  res._parentWrap = wrap; // JS object: Socket, JSStreamSocket, ...
  res._secureContext = context;
  res.reading = handle.reading;
  this[kRes] = res;
  defineHandleReading(this, handle);

  // Guard against adding multiple listeners, as this method may be called
  // repeatedly on the same socket by reinitializeHandle
  if (this.listenerCount('close', onSocketCloseDestroySSL) === 0) {
    this.on('close', onSocketCloseDestroySSL);
  }

  if (wrap) {
    wrap.on('close', () => this.destroy());
  }

  return res;
};

TLSSocket.prototype[kReinitializeHandle] = function reinitializeHandle(
  this: TLSSocketClass,
  handle: unknown
) {
  const originalServername = this.ssl ? this._handle.getServername() : null;
  const originalSession = this.ssl ? this._handle.getSession() : null;

  this.handle = this._wrapHandle(null, handle, false);
  this.ssl = this._handle;

  Socket.prototype[kReinitializeHandle].call(this, this.handle);
  this._init();

  if (this._tlsOptions.enableTrace) {
    this._handle.enableTrace();
  }

  if (originalSession) {
    this.setSession(originalSession);
  }

  if (originalServername) {
    this.setServername(originalServername);
  }
};

// This eliminates a cyclic reference to TLSWrap
// Ref: https://github.com/nodejs/node/commit/f7620fb96d339f704932f9bb9a0dceb9952df2d4
function defineHandleReading(
  socket: TLSSocketClass,
  handle: TLSSocketClass['_handle']
) {
  Object.defineProperty(handle, 'reading', {
    get: (): boolean => {
      return socket[kRes].reading;
    },
    set: (value: boolean): void => {
      socket[kRes].reading = value;
    },
  });
}

function onSocketCloseDestroySSL(this: TLSSocketClass): void {
  // Make sure we are not doing it on OpenSSL's stack
  setImmediate(destroySSL, this);
  this[kRes] = null;
}

function destroySSL(self: TLSSocketClass) {
  self._destroySSL();
}

TLSSocket.prototype._destroySSL = function _destroySSL(): void {
  if (!this.ssl) return;
  this.ssl.destroySSL();
  if (this.ssl._secureContext.singleUse) {
    this.ssl._secureContext.context.close();
    this.ssl._secureContext.context = null;
  }
  this.ssl = null;
  this[kPendingSession] = null;
  this[kIsVerified] = false;
};

function keylogNewListener(this: TLSSocketClass, event: string): void {
  if (event !== 'keylog') return;

  // Guard against enableKeylogCallback after destroy
  if (!this._handle) return;
  this._handle.enableKeylogCallback();

  // Remove this listener since it's no longer needed.
  this.removeListener('newListener', keylogNewListener);
}

function newListener(this: TLSSocketClass, event: string): void {
  if (event !== 'session') return;

  // Guard against enableSessionCallbacks after destroy
  if (!this._handle) return;
  this._handle.enableSessionCallbacks();

  // Remove this listener since it's no longer needed.
  this.removeListener('newListener', newListener);
}

TLSSocket.prototype._init = function _init(
  this: TLSSocketClass,
  socket: typeof Socket,
  wrap: unknown
): void {
  const options = this._tlsOptions;
  const ssl = this._handle;
  this.server = options.server;

  // Clients (!isServer) always request a cert, servers request a client cert
  // only on explicit configuration.
  const requestCert = !!options.requestCert || !options.isServer;
  const rejectUnauthorized = !!options.rejectUnauthorized;

  this._requestCert = requestCert;
  this._rejectUnauthorized = rejectUnauthorized;
  if (requestCert || rejectUnauthorized)
    ssl.setVerifyMode(requestCert, rejectUnauthorized);

  // Only call .onkeylog if there is a keylog listener.
  ssl.onkeylog = onkeylog;

  if (this.listenerCount('newListener', keylogNewListener) === 0) {
    this.on('newListener', keylogNewListener);
  }

  if (options.isServer) {
    // This option is not supported.
    // ssl.onhandshakestart = onhandshakestart;
    // ssl.onhandshakedone = onhandshakedone;
    // ssl.onclienthello = loadSession;
    // ssl.oncertcb = loadSNI;
    // ssl.onnewsession = onnewsession;
    // ssl.lastHandshakeTime = 0;
    // ssl.handshakes = 0;
    //
    // if (options.ALPNCallback) {
    //   validateFunction(options.ALPNCallback, 'options.ALPNCallback');
    //   this[kALPNCallback] = options.ALPNCallback;
    //   ssl.ALPNCallback = callALPNCallback;
    //   ssl.enableALPNCb();
    // }
    //
    // if (this.server) {
    //   if (
    //     this.server.listenerCount('resumeSession') > 0 ||
    //     this.server.listenerCount('newSession') > 0
    //   ) {
    //     // Also starts the client hello parser as a side effect.
    //     ssl.enableSessionCallbacks();
    //   }
    //   if (this.server.listenerCount('OCSPRequest') > 0) ssl.enableCertCb();
    // }
  } else {
    ssl.onhandshakestart = noop;
    ssl.onhandshakedone = () => {
      this._finishInit();
    };
    ssl.onocspresponse = onocspresponse;

    if (options.session) ssl.setSession(options.session);

    ssl.onnewsession = onnewsessionclient;

    // Only call .onnewsession if there is a session listener.
    if (this.listenerCount('newListener', newListener) === 0) {
      this.on('newListener', newListener);
    }
  }

  // We do not support tls key logging.
  //
  // if (tlsKeylog) {
  //   this.on('keylog', (line) => {
  //     appendFile(tlsKeylog, line, { mode: 0o600 }, (err) => {});
  //   });
  // }

  ssl.onerror = onerror;

  // If custom SNICallback was given, or if
  // there're SNI contexts to perform match against -
  // set `.onsniselect` callback.
  if (
    options.isServer &&
    options.SNICallback &&
    (options.SNICallback !== SNICallback ||
      (options.server && options.server._contexts.length))
  ) {
    validateFunction(options.SNICallback, 'options.SNICallback');
    this._SNICallback = options.SNICallback;
    ssl.enableCertCb();
  }

  if (options.ALPNProtocols) ssl.setALPNProtocols(options.ALPNProtocols);

  if (options.pskCallback && ssl.enablePskCallback) {
    validateFunction(options.pskCallback, 'pskCallback');

    ssl[kOnPskExchange] = options.isServer
      ? onPskServerCallback
      : onPskClientCallback;

    this[kPskCallback] = options.pskCallback;
    ssl.enablePskCallback();

    if (options.pskIdentityHint) {
      validateString(options.pskIdentityHint, 'options.pskIdentityHint');
      ssl.setPskIdentityHint(options.pskIdentityHint);
    }
  }

  // We can only come here via [kWrapConnectedHandle]() call that happens
  // if the connection is established with `autoSelectFamily` set to `true`.
  const connectOptions = this[kConnectOptions];
  if (!options.isServer && connectOptions) {
    if (connectOptions.servername) {
      this.setServername(connectOptions.servername);
    }
  }

  if (options.handshakeTimeout > 0)
    this.setTimeout(options.handshakeTimeout, this._handleTimeout);

  if (socket instanceof Socket) {
    this._parent = socket;

    // To prevent assertion in afterConnect() and properly kick off readStart
    this.connecting = socket.connecting || !socket._handle;
    socket.once('connect', () => {
      this.connecting = false;
      this.emit('connect');
    });
  }

  // Assume `tls.connect()`
  if (wrap) {
    wrap.on('error', (err) => this._emitTLSError(err));
  } else {
    assert(!socket);
    this.connecting = true;
  }
};

TLSSocket.prototype.renegotiate = function (
  this: TLSSocketClass,
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

  if (this.destroyed) return false;

  let requestCert = !!this._requestCert;
  let rejectUnauthorized = !!this._rejectUnauthorized;

  if (options.requestCert !== undefined) requestCert = !!options.requestCert;
  if (options.rejectUnauthorized !== undefined)
    rejectUnauthorized = !!options.rejectUnauthorized;

  if (
    requestCert !== this._requestCert ||
    rejectUnauthorized !== this._rejectUnauthorized
  ) {
    this._handle.setVerifyMode(requestCert, rejectUnauthorized);
    this._requestCert = requestCert;
    this._rejectUnauthorized = rejectUnauthorized;
  }
  // Ensure that we'll cycle through internal openssl's state
  this.write('');

  try {
    this._handle.renegotiate();
  } catch (err) {
    if (callback) {
      process.nextTick(callback, err);
    }
    return false;
  }

  // Ensure that we'll cycle through internal openssl's state
  this.write('');

  if (callback) {
    this.once('secure', () => callback(null));
  }

  return true;
};

TLSSocket.prototype.exportKeyingMaterial = function (length, label, context) {
  validateUint32(length, 'length', true);
  validateString(label, 'label');
  if (context !== undefined) validateBuffer(context, 'context');

  if (!this._secureEstablished) throw new ERR_TLS_INVALID_STATE();

  return this._handle.exportKeyingMaterial(length, label, context);
};

TLSSocket.prototype.setMaxSendFragment = function setMaxSendFragment(size) {
  validateInt32(size, 'size');
  return this._handle.setMaxSendFragment(size) === 1;
};

TLSSocket.prototype._handleTimeout = function () {
  this._emitTLSError(new ERR_TLS_HANDSHAKE_TIMEOUT());
};

TLSSocket.prototype._emitTLSError = function (err) {
  const e = this._tlsError(err);
  if (e) this.emit('error', e);
};

TLSSocket.prototype._tlsError = function (err) {
  this.emit('_tlsError', err);
  if (this._controlReleased) return err;
  return null;
};

TLSSocket.prototype._releaseControl = function () {
  if (this._controlReleased) return false;
  this._controlReleased = true;
  this.removeListener('error', this._tlsError);
  return true;
};

TLSSocket.prototype._finishInit = function () {
  // Guard against getting onhandshakedone() after .destroy().
  // * 1.2: If destroy() during onocspresponse(), then write of next handshake
  // record fails, the handshake done info callbacks does not occur, and the
  // socket closes.
  // * 1.3: The OCSP response comes in the same record that finishes handshake,
  // so even after .destroy(), the handshake done info callback occurs
  // immediately after onocspresponse(). Ignore it.
  if (!this._handle) return;

  this.alpnProtocol = this._handle.getALPNNegotiatedProtocol();
  // The servername could be set by TLSWrap::SelectSNIContextCallback().
  if (this.servername === null) {
    this.servername = this._handle.getServername();
  }

  debug(
    '%s _finishInit',
    this._tlsOptions.isServer ? 'server' : 'client',
    'handle?',
    !!this._handle,
    'alpn',
    this.alpnProtocol,
    'servername',
    this.servername
  );

  this._secureEstablished = true;
  if (this._tlsOptions.handshakeTimeout > 0)
    this.setTimeout(0, this._handleTimeout);
  this.emit('secure');
};

TLSSocket.prototype._start = function () {
  debug(
    '%s _start',
    this._tlsOptions.isServer ? 'server' : 'client',
    'handle?',
    !!this._handle,
    'connecting?',
    this.connecting,
    'requestOCSP?',
    !!this._tlsOptions.requestOCSP
  );
  if (this.connecting) {
    this.once('connect', this._start);
    return;
  }

  // Socket was destroyed before the connection was established
  if (!this._handle) return;

  if (this._tlsOptions.requestOCSP) this._handle.requestOCSP();
  this._handle.start();
};

TLSSocket.prototype.setServername = function (name: string) {
  validateString(name, 'name');

  if (this._tlsOptions.isServer) {
    throw new ERR_TLS_SNI_FROM_SERVER();
  }

  this._handle.setServername(name);
};

TLSSocket.prototype.setSession = function (session) {
  if (typeof session === 'string') session = Buffer.from(session, 'latin1');
  this._handle.setSession(session);
};

TLSSocket.prototype.getPeerCertificate = function (detailed) {
  if (this._handle) {
    return (
      common.translatePeerCertificate(
        this._handle.getPeerCertificate(detailed)
      ) || {}
    );
  }

  return null;
};

TLSSocket.prototype.getCertificate = function (
  this: TLSSocketClass
): ReturnType<TLSSocketType['getCertificate']> {
  if (this._handle) {
    // It's not a peer cert, but the formatting is identical.
    return common.translatePeerCertificate(this._handle.getCertificate()) || {};
  }

  return null;
};

TLSSocket.prototype.getPeerX509Certificate = function (
  detailed
): ReturnType<TLSSocketType['getPeerX509Certificate']> {
  const cert = this._handle?.getPeerX509Certificate();
  return cert ? new InternalX509Certificate(cert) : undefined;
};

TLSSocket.prototype.getX509Certificate = function (): ReturnType<
  TLSSocketType['getX509Certificate']
> {
  const cert = this._handle?.getX509Certificate();
  return cert ? new InternalX509Certificate(cert) : undefined;
};

TLSSocket.prototype.setKeyCert = function (context): void {
  if (this._handle) {
    let secureContext;
    if (context instanceof common.SecureContext) secureContext = context;
    else secureContext = tls.createSecureContext(context);
    this._handle.setKeyCert(secureContext.context);
  }
};

// Proxy TLSSocket handle methods
function makeSocketMethodProxy(name: string) {
  return function socketMethodProxy(
    this: TLSSocketClass,
    ...args: unknown[]
  ): unknown | null {
    if (this._handle)
      return Reflect.apply(this._handle[name], this._handle, args);
    return null;
  };
}

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
  TLSSocket.prototype[method] = makeSocketMethodProxy(method);
});

function SNICallback(
  servername: string,
  callback: (error: Error | null, data?: string) => void
): void {
  const contexts = this.server._contexts;

  for (let i = contexts.length - 1; i >= 0; --i) {
    const elem = contexts[i];
    if (elem[0].test(servername)) {
      callback(null, elem[1]);
      return;
    }
  }

  callback(null, undefined);
}

// Target API:
//
//  let s = tls.connect({port: 8000, host: "google.com"}, function() {
//    if (!s.authorized) {
//      s.destroy();
//      return;
//    }
//
//    // s.socket;
//
//    s.end("hello world\n");
//  });
//
//
function normalizeConnectArgs(
  listArgs: unknown[]
): [ConnectionOptions] | [ConnectionOptions, VoidFunction] {
  const args = _normalizeArgs(listArgs);
  const options = args[0] as ConnectionOptions;
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

function onConnectSecure(this: TLSSocketClass): void {
  const options = this[kConnectOptions];

  // Check the size of DHE parameter above minimum requirement
  // specified in options.
  const ekeyinfo = this.getEphemeralKeyInfo();
  if (ekeyinfo.type === 'DH' && ekeyinfo.size < options.minDHSize) {
    const err = new ERR_TLS_DH_PARAM_SIZE(ekeyinfo.size);
    this.emit('error', err);
    this.destroy();
    return;
  }

  let verifyError = this._handle.verifyError();

  // Verify that server's identity matches it's certificate's names
  // Unless server has resumed our existing session
  if (!verifyError && !this.isSessionReused()) {
    const hostname =
      options.servername ||
      options.host ||
      options.socket?._host ||
      'localhost';
    const cert = this.getPeerCertificate(true);
    verifyError = options.checkServerIdentity(hostname, cert);
  }

  if (verifyError) {
    this.authorized = false;
    this.authorizationError = verifyError.code || verifyError.message;

    // rejectUnauthorized property can be explicitly defined as `undefined`
    // causing the assignment to default value (`true`) fail. Before assigning
    // it to the tlssock connection options, explicitly check if it is false
    // and update rejectUnauthorized property. The property gets used by
    // TLSSocket connection handler to allow or reject connection if
    // unauthorized.
    // This check is potentially redundant, however it is better to keep it
    // in case the option object gets modified somewhere.
    if (options.rejectUnauthorized !== false) {
      this.destroy(verifyError);
      return;
    }
  } else {
    this.authorized = true;
  }
  this.secureConnecting = false;
  this.emit('secureConnect');

  this[kIsVerified] = true;
  const session = this[kPendingSession];
  this[kPendingSession] = null;
  if (session) this.emit('session', session);

  this.removeListener('end', onConnectEnd);
}

function onConnectEnd(this: TLSSocketClass): void {
  // NOTE: This logic is shared with _http_client.js
  if (!this._hadError) {
    const options = this[kConnectOptions];
    this._hadError = true;
    const error = new ConnResetException(
      'Client network socket disconnected ' +
        'before secure TLS connection was ' +
        'established'
    );
    error.path = options.path;
    error.host = options.host;
    error.port = options.port;
    error.localAddress = options.localAddress;
    this.destroy(error);
  }
}

// Arguments: [port,] [host,] [options,] [cb]
export function connect(...args: unknown[]): TLSSocketClass {
  args = normalizeConnectArgs(args);
  let options = args[0] as ConnectionOptions;
  const cb = args[1] as VoidFunction;

  options = {
    // Workers does not support this option deliberately.
    rejectUnauthorized: true,
    ciphers: tls.DEFAULT_CIPHERS,
    checkServerIdentity: tls.checkServerIdentity,
    minDHSize: 1024,
    ...options,
  };

  if (!options.keepAlive) options.singleUse = true;

  validateFunction(options.checkServerIdentity, 'options.checkServerIdentity');
  validateNumber(options.minDHSize, 'options.minDHSize', 1);

  const context = options.secureContext || tls.createSecureContext(options);

  const tlssock = new TLSSocket(options.socket, {
    allowHalfOpen: options.allowHalfOpen,
    pipe: !!options.path,
    secureContext: context,
    isServer: false,
    requestCert: true,
    rejectUnauthorized: options.rejectUnauthorized !== false,
    session: options.session,
    ALPNProtocols: options.ALPNProtocols,
    requestOCSP: options.requestOCSP,
    enableTrace: options.enableTrace,
    pskCallback: options.pskCallback,
    highWaterMark: options.highWaterMark,
    onread: options.onread,
    signal: options.signal,
  });

  // rejectUnauthorized property can be explicitly defined as `undefined`
  // causing the assignment to default value (`true`) fail. Before assigning
  // it to the tlssock connection options, explicitly check if it is false
  // and update rejectUnauthorized property. The property gets used by TLSSocket
  // connection handler to allow or reject connection if unauthorized
  options.rejectUnauthorized = options.rejectUnauthorized !== false;

  tlssock[kConnectOptions] = options;

  if (cb) tlssock.once('secureConnect', cb);

  if (!options.socket) {
    // If user provided the socket, it's their responsibility to manage its
    // connectivity. If we created one internally, we connect it.
    if (options.timeout) {
      tlssock.setTimeout(options.timeout);
    }

    tlssock.connect(options, tlssock._start);
  }

  tlssock._releaseControl();

  if (options.session) tlssock.setSession(options.session);

  if (options.servername) {
    tlssock.setServername(options.servername);
  }

  if (options.socket) tlssock._start();

  tlssock.on('secure', onConnectSecure);
  tlssock.prependListener('end', onConnectEnd);

  return tlssock;
}
