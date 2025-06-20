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

/* eslint-disable @typescript-eslint/no-redundant-type-constituents */

import { notStrictEqual } from 'node-internal:internal_assert';
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
  stream: Duplex;
  [kCurrentWriteRequest]: null | unknown;
  [kCurrentShutdownRequest]: null | unknown;
  [kPendingShutdownRequest]: null | unknown;
  [kPendingClose]: boolean;

  constructor(stream: Duplex) {
    // eslint-disable-next-line @typescript-eslint/no-invalid-void-type
    const closePromise = Promise.withResolvers<void>();
    const openPromise = Promise.withResolvers<SocketInfo>();

    const webStream = toBYOBWeb(stream);
    Object.assign(webStream.writable, {
      // eslint-disable-next-line @typescript-eslint/require-await
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
      // eslint-disable-next-line @typescript-eslint/no-unsafe-argument
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
      // eslint-disable-next-line @typescript-eslint/no-unnecessary-boolean-literal-compare
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

    // eslint-disable-next-line @typescript-eslint/no-floating-promises
    handle.socket.closed.then(this.doClose.bind(this));

    openPromise.resolve({});

    // Start reading.
    this.read(0);
  }

  isClosing(): boolean {
    return !this.readable || !this.writable;
  }

  readStart(): number {
    this.stream.resume();
    return 0;
  }

  readStop(): number {
    this.stream.pause();
    return 0;
  }

  doShutdown(req: unknown): number {
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
      this.stream.end();
    });
    return 0;
  }

  doClose(): void {
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
      notStrictEqual(handle, null);
      this[kPendingClose] = false;
    });
  }
}
