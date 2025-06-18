// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import type { IncomingMessage as _IncomingMessage } from 'node:http';
import { Readable } from 'node-internal:streams_readable';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export class IncomingMessage extends Readable implements _IncomingMessage {
  #reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
  #reading: boolean = false;
  #response: Response;
  #resetTimers: (opts: { finished: boolean }) => void;
  #aborted: boolean = false;

  url: string = '';
  // @ts-expect-error TS2416 Type-inconsistencies
  method: string | null = null;
  // @ts-expect-error TS2416 Type-inconsistencies
  statusCode: number | null = null;
  // @ts-expect-error TS2416 Type-inconsistencies
  statusMessage: string | null = null;
  httpVersionMajor = 1;
  httpVersionMinor = 1;
  httpVersion: string = '1.1';

  // TODO(soon): Get rid of the second argument.
  constructor(
    response: Response,
    resetTimers?: (opts: { finished: boolean }) => void
  ) {
    if (!(response instanceof Response) || resetTimers == null) {
      // IncomingMessage constructor is not documented by Node.js but in order
      // to be 100% node.js compatible we need to implement it, and expose it as
      // a class that can be constructed.
      //
      // Node.js uses "Socket" as the first argument for IncomingMessage and
      // does not have a second argument. In order to implement our "fetch"
      // based ClientRequest class, we need to implement it as following.
      //
      // TODO(soon): Try to remove these from the constructor.
      throw new ERR_METHOD_NOT_IMPLEMENTED('IncomingMessage');
    }
    super({});

    this.#resetTimers = resetTimers;
    this.#response = response;

    if (this._readableState) {
      this._readableState.readingMore = true;
    }

    this.url = response.url;
    this.statusCode = response.status;
    this.statusMessage = response.statusText;

    this.once('end', () => {
      queueMicrotask(() => this.emit('close'));
    });

    this.on('timeout', () => {
      this.#reading = false;
    });

    this.#reader = this.#response.body?.getReader();
    // eslint-disable-next-line @typescript-eslint/no-floating-promises
    this.#tryRead();
  }

  async #tryRead(): Promise<void> {
    if (!this.#reader || this.#reading || this.#aborted) return;

    this.#reading = true;

    try {
      // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
      while (true) {
        const { done, value } = await this.#reader.read();
        if (done) {
          break;
        }
        this.push(value);
      }
    } catch (error) {
      this.emit('error', error);
    } finally {
      this.#reading = false;
      this.#resetTimers({ finished: true });
      this.push(null);
    }
  }

  override _read(): void {
    if (!this.#reading && !this.#aborted) {
      // eslint-disable-next-line @typescript-eslint/no-floating-promises
      this.#tryRead();
    }
  }

  override _destroy(
    error: Error | null,
    callback: (error?: Error | null) => void
  ): void {
    if (!this.#aborted) {
      this.#aborted = true;
      this.emit('aborted');
    }
    this.#reading = false;

    queueMicrotask(() => {
      callback(error);
    });
  }

  get headers(): Record<string, string> {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument,@typescript-eslint/no-explicit-any
    return Object.fromEntries(this.#response.headers as any);
  }

  // @ts-expect-error TS2416 Type inconsistency
  get headersDistinct(): Record<string, string> {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument,@typescript-eslint/no-explicit-any
    return Object.fromEntries(this.#response.headers as any);
  }

  // @ts-expect-error TS2416 Type inconsistency
  get trailers(): Record<string, unknown> {
    // Not supported.
    return {};
  }

  // @ts-expect-error TS2416 Type inconsistency
  get trailersDistinct(): Record<string, unknown> {
    // Not supported.
    return {};
  }

  setTimeout(_msecs: number, callback?: () => void): this {
    if (callback) {
      this.on('timeout', callback);
    }
    return this;
  }
}
