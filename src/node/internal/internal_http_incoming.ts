import type { IncomingMessage as _IncomingMessage } from 'node:http';
import { Readable } from 'node-internal:streams_readable';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export class IncomingMessage extends Readable implements _IncomingMessage {
  #resume: VoidFunction | null = null;
  #response: Response;

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

    const writable = new WritableStream({
      write: (chunk): Promise<void> => {
        resetTimers({ finished: false });
        return new Promise((resolve, reject) => {
          if (this.destroyed) {
            // TODO(soon): Throw proper error here.
            reject(new Error('Destroyed'));
          } else if (this.push(Buffer.from(chunk))) {
            resolve();
          } else {
            this.#resume = resolve;
          }
        });
      },
      close: (): void => {
        resetTimers({ finished: true });
        if (!this.destroyed) {
          this.push(null);
        }
      },
      abort: (err: Error): void => {
        resetTimers({ finished: true });
        if (!this.destroyed) {
          this.emit('error', err);
        }
      },
    });

    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument
    response.body?.pipeTo(writable).catch((err: unknown) => {
      resetTimers({ finished: true });
      if (!this.destroyed) {
        this.emit('error', err);
      }
    });
  }

  public override _read(): void {
    this.#resume?.();
    this.#resume = null;
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
