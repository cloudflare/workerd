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

import { ok, strictEqual, notStrictEqual } from 'node-internal:internal_assert';
import { Socket } from 'node-internal:internal_net';
import { ERR_STREAM_WRAP } from 'node-internal:internal_errors';
import { Duplex, toBYOBWeb } from 'node-internal:streams_duplex';
import type {
  SocketInfo,
  Writer,
  Socket as CloudflareSocket,
} from 'node-internal:sockets';

const kCurrentWriteRequest = Symbol('kCurrentWriteRequest');
const kCurrentShutdownRequest = Symbol('kCurrentShutdownRequest');
const kPendingShutdownRequest = Symbol('kPendingShutdownRequest');
const kPendingClose = Symbol('kPendingClose');

/* This class serves as a wrapper for when the C++ side of Node wants access
 * to a standard JS stream. For example, TLS or HTTP do not operate on network
 * resources conceptually, although that is the common case and what we are
 * optimizing for; in theory, they are completely composable and can work with
 * any stream resource they see.
 *
 * For the common case, i.e. a TLS socket wrapping around a net.Socket, we
 * can skip going through the JS layer and let TLS access the raw C++ handle
 * of a net.Socket. The flipside of this is that, to maintain composability,
 * we need a way to create "fake" net.Socket instances that call back into a
 * "real" JavaScript stream. JSStreamSocket is exactly this.
 */
export class JSStreamSocket extends Socket {
  public stream: Duplex;
  public [kCurrentWriteRequest]: null | unknown;
  public [kCurrentShutdownRequest]: null | unknown;
  public [kPendingShutdownRequest]: null | unknown;
  public [kPendingClose]: boolean;

  constructor(stream: Duplex) {
    const closePromise = Promise.withResolvers<void>();
    const openPromise = Promise.withResolvers<SocketInfo>();

    const webStream = toBYOBWeb(stream);
    Object.assign(webStream.writable, {
      write: async (data: string | ArrayBufferView): Promise<void> => {
        stream.write(data);
      },
      closed: closePromise.promise,
      releaseLock: async (): Promise<void> => {},
    });
    const handle: Socket['_handle'] = {
      reading: true,
      bytesRead: 0,
      bytesWritten: 0,
      socket: {
        startTls(): CloudflareSocket {
          throw new Error(
            'startTls() should not be called for a duplex stream'
          );
        },
        upgraded: false,
        secureTransport: 'off',
        closed: closePromise.promise,
        close: async (): Promise<void> => {
          queueMicrotask(() => {
            closePromise.resolve();
          });
          return closePromise.promise;
        },
        opened: openPromise.promise,
        readable: webStream.readable,
        writable: webStream.writable as unknown as Writer,
      },
      reader: new ReadableStreamBYOBReader(webStream.readable),
      writer: new WritableStreamDefaultWriter<unknown>(webStream.writable),
      options: {
        host: '0.0.0.0',
        port: 0,
        addressType: 4,
      },
    };

    stream.pause();
    stream.on('error', (err) => this.emit('error', err));
    const ondata = (chunk: string | Buffer): void => {
      if (typeof chunk === 'string' || stream.readableObjectMode === true) {
        // Make sure that no further `data` events will happen.
        stream.pause();
        stream.removeListener('data', ondata);

        this.emit('error', new ERR_STREAM_WRAP());
        return;
      }

      // TODO(soon): We need to trigger read() result for _handle.reader.read(buf) call
      // in node:net.
    };
    stream.on('data', ondata);
    stream.once('end', () => {
      closePromise.resolve();
    });
    // Some `Stream` don't pass `hasError` parameters when closed.
    stream.once('close', () => {
      // Errors emitted from `stream` have also been emitted to this instance
      // so that we don't pass errors to `destroy()` again.
      this.destroy();
    });

    super({ handle });
    this.stream = stream;
    this[kCurrentWriteRequest] = null;
    this[kCurrentShutdownRequest] = null;
    this[kPendingShutdownRequest] = null;
    this[kPendingClose] = false;
    this.readable = stream.readable;
    // @ts-expect-error TS2540 Read-only property.
    this.writable = stream.writable;

    handle?.socket?.closed.then(this.doClose.bind(this));

    openPromise.resolve({});

    // Start reading.
    this.read(0);
  }

  // Allow legacy requires in the test suite to keep working:
  //   const { StreamWrap } = require('internal/js_stream_socket')
  static get StreamWrap() {
    return JSStreamSocket;
  }

  public isClosing(): boolean {
    return !this.readable || !this.writable;
  }

  public readStart(): number {
    this.stream.resume();
    return 0;
  }

  public readStop(): number {
    this.stream.pause();
    return 0;
  }

  public doShutdown(req: unknown) {
    // TODO(addaleax): It might be nice if we could get into a state where
    // DoShutdown() is not called on streams while a write is still pending.
    //
    // Currently, the only part of the code base where that happens is the
    // TLS implementation, which calls both DoWrite() and DoShutdown() on the
    // underlying network stream inside of its own DoShutdown() method.
    // Working around that on the native side is not quite trivial (yet?),
    // so for now that is supported here.

    if (this[kCurrentWriteRequest] !== null) {
      this[kPendingShutdownRequest] = req;
      return 0;
    }

    ok(this[kCurrentWriteRequest] === null);
    ok(this[kCurrentShutdownRequest] === null);
    this[kCurrentShutdownRequest] = req;

    if (this[kPendingClose]) {
      // If doClose is pending, the stream & this._handle are gone. We can't do
      // anything. doClose will call finishShutdown with ECANCELED for us shortly.
      return 0;
    }

    const handle = this._handle;
    notStrictEqual(handle, null);

    queueMicrotask(() => {
      // Ensure that write is dispatched asynchronously.
      this.stream.end(() => {
        this.finishShutdown(handle, null);
      });
    });
    return 0;
  }

  // handle === this._handle except when called from doClose().
  public finishShutdown(
    _handle: JSStreamSocket['_handle'],
    _errCode: string | null
  ): void {
    // The shutdown request might already have been cancelled.
    if (this[kCurrentShutdownRequest] === null) return;
    // const req = this[kCurrentShutdownRequest];
    this[kCurrentShutdownRequest] = null;
    // handle.finishShutdown(req, errCode);
  }

  public doWrite(req: unknown, bufs: Buffer[]): number {
    strictEqual(this[kCurrentWriteRequest], null);
    strictEqual(this[kCurrentShutdownRequest], null);

    if (this[kPendingClose]) {
      // If doClose is pending, the stream & this._handle are gone. We can't do
      // anything. doClose will call finishWrite with ECANCELED for us shortly.
      this[kCurrentWriteRequest] = req; // Store req, for doClose to cancel
      return 0;
    } else if (this._handle === null) {
      // If this._handle is already null, there is nothing left to do with a
      // pending write request, so we discard it.
      return 0;
    }

    const handle = this._handle;

    const self = this;

    let pending = bufs.length;

    this.stream.cork();
    // Use `var` over `let` for performance optimization.
    // eslint-disable-next-line no-var
    for (var i = 0; i < bufs.length; ++i) this.stream.write(bufs[i], done);
    this.stream.uncork();

    // Only set the request here, because the `write()` calls could throw.
    this[kCurrentWriteRequest] = req;

    function done(err: Error | null | undefined): void {
      if (!err && --pending !== 0) return;

      // Ensure that this is called once in case of error
      pending = 0;

      let errCode: string | null;
      if (err) {
        // @ts-expect-error TS2339 NodeError vs. Error difference.
        errCode = err.code;
      }

      // Ensure that write was dispatched
      setImmediate(() => {
        self.finishWrite(handle, errCode);
      });
    }

    return 0;
  }

  // handle === this._handle except when called from doClose().
  public finishWrite(
    _handle: JSStreamSocket['_handle'],
    _errCode: string | null
  ): void {
    // The write request might already have been cancelled.
    if (this[kCurrentWriteRequest] === null) return;
    // const req = this[kCurrentWriteRequest];
    this[kCurrentWriteRequest] = null;

    // handle.finishWrite(req, errCode);
    if (this[kPendingShutdownRequest]) {
      const req = this[kPendingShutdownRequest];
      this[kPendingShutdownRequest] = null;
      this.doShutdown(req);
    }
  }

  public doClose(): void {
    this[kPendingClose] = true;

    const handle = this._handle;

    // When sockets of the "net" module destroyed, they will call
    // `this._handle.close()` which will also emit EOF if not emitted before.
    // This feature makes sockets on the other side emit "end" and "close"
    // even though we haven't called `end()`. As `stream` are likely to be
    // instances of `net.Socket`, calling `stream.destroy()` manually will
    // avoid issues that don't properly close wrapped connections.
    this.stream.destroy();

    queueMicrotask(() => {
      this.finishWrite(handle, 'UV_ECANCELED');
      this.finishShutdown(handle, 'UV_ECANCELED');

      this[kPendingClose] = false;
    });
  }
}
