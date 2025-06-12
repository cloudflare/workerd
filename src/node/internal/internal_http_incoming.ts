import type { IncomingMessage as _IncomingMessage } from 'node:http';
import { Readable } from 'node-internal:streams_readable';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export class IncomingMessage extends Readable implements _IncomingMessage {
  #reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
  #reading: boolean = false;
  #response: Response;
  #resetTimers: (opts: { finished: boolean }) => void;

  public url: string = '';
  // @ts-expect-error TS2416 Type-inconsistencies
  public method: string | null = null;
  // @ts-expect-error TS2416 Type-inconsistencies
  public statusCode: number | null = null;
  // @ts-expect-error TS2416 Type-inconsistencies
  public statusMessage: string | null = null;
  public httpVersionMajor = 1;
  public httpVersionMinor = 1;
  public httpVersion: string = '1.1';

  // TODO(soon): Get rid of the second argument.
  public constructor(
    response: Response,
    resetTimers: (opts: { finished: boolean }) => void
  ) {
    if (!(response instanceof Response)) {
      // TODO(soon): Fully support IncomingMessage constructor
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

    this.on('end', () => {
      queueMicrotask(() => this.emit('close'));
    });

    this.#reader = this.#response.body?.getReader();
    // eslint-disable-next-line @typescript-eslint/no-floating-promises
    this.#tryRead();
  }

  async #tryRead(): Promise<void> {
    if (!this.#reader || this.#reading) return;

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

  public override _read(): void {
    if (!this.#reading) {
      // eslint-disable-next-line @typescript-eslint/no-floating-promises
      this.#tryRead();
    }
  }

  public get headers(): Record<string, string> {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument,@typescript-eslint/no-explicit-any
    return Object.fromEntries(this.#response.headers as any);
  }

  // @ts-expect-error TS2416 Type inconsistency
  public get headersDistinct(): Record<string, string> {
    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument,@typescript-eslint/no-explicit-any
    return Object.fromEntries(this.#response.headers as any);
  }

  // @ts-expect-error TS2416 Type inconsistency
  public get trailers(): Record<string, unknown> {
    // Not supported.
    return {};
  }

  // @ts-expect-error TS2416 Type inconsistency
  public get trailersDistinct(): Record<string, unknown> {
    // Not supported.
    return {};
  }

  public setTimeout(_msecs: number, _callback?: () => void): this {
    // TODO(soon): Not yet implemented
    return this;
  }
}
