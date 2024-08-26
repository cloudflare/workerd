// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import entrypoints from 'cloudflare-internal:workers';

async function* readLines(
  stream: ReadableStream<string>
): AsyncGenerator<string> {
  let start = 0;
  let end = 0;
  let partial = '';

  // @ts-expect-error must have a '[Symbol.asyncIterator]()' method
  for await (const chunk of stream) {
    const full = partial + chunk;
    for (const char of full) {
      if (char === '\n') {
        yield full.substring(start, end);
        end++;
        start = end;
      } else {
        end++;
      }
    }

    partial = full.substring(start, end);
    start = 0;
    end = 0;
  }

  if (partial.length > 0) {
    yield partial;
  }
}

type Batch = {
  id: string; // unique identifier for the batch
  shard: string; // assigned shard
  ts: number; // creation timestamp of the batch

  format: Format;
  size: {
    bytes: number;
    rows: number;
  };
  data: unknown;
};

type JsonStream = Batch & {
  format: Format.JSON_STREAM;
  data: ReadableStream<Uint8Array>;
};

enum Format {
  JSON_STREAM = 'json_stream', // jsonl
}

export class PipelineTransformImpl extends entrypoints.WorkerEntrypoint {
  #batch?: Batch;
  #initalized: boolean = false;

  // stub overriden on the sub class
  // eslint-disable-next-line @typescript-eslint/require-await
  public async transformJson(_data: object[]): Promise<object[]> {
    throw new Error('should be implemented by parent');
  }

  // called by the dispatcher which then calls the subclass methods
  // @ts-expect-error thinks ping is never used
  private async _ping(): Promise<void> {
    // making sure the function was overriden by an implementing subclass
    if (this.transformJson !== PipelineTransformImpl.prototype.transformJson) {
      return await Promise.resolve(); // eslint
    } else {
      throw new Error(
        'the transformJson method must be overridden by the PipelineTransform subclass'
      );
    }
  }

  // called by the dispatcher which then calls the subclass methods
  // the reason this is typescript private and not javascript private is that this must be
  // able to be called by the dispatcher but should not be called by the class implementer
  // @ts-expect-error _transform is called by rpc
  private async _transform(batch: Batch): Promise<JsonStream> {
    if (this.#initalized) {
      throw new Error('pipeline entrypoint has already been initialized');
    }

    this.#batch = batch;
    this.#initalized = true;

    switch (this.#batch.format) {
      case Format.JSON_STREAM:
        const data = await this.#readJsonStream();
        const transformed = await this.transformJson(data);
        return this.#sendJson(transformed);
      default:
        throw new Error('unsupported batch format');
    }
  }

  async #readJsonStream(): Promise<object[]> {
    if (this.#batch!.format !== Format.JSON_STREAM) {
      throw new Error(`expected JSON_STREAM not ${this.#batch!.format}`);
    }

    const batch = this.#batch!.data as ReadableStream<Uint8Array>;
    const decoder = batch.pipeThrough(new TextDecoderStream());

    const data: object[] = [];
    for await (const line of readLines(decoder)) {
      data.push(JSON.parse(line) as object);
    }

    return data;
  }

  #sendJson(data: object[]): JsonStream {
    if (!(data instanceof Array)) {
      throw new Error('transformJson must return an array of objects');
    }

    let written = 0;
    const encoder = new TextEncoder();
    const readable = new ReadableStream<Uint8Array>({
      start(controller): void {
        for (const obj of data) {
          const encoded = encoder.encode(`${JSON.stringify(obj)}\n`);
          written += encoded.length;
          controller.enqueue(encoded);
        }

        controller.close();
      },
    });

    return {
      id: this.#batch!.id,
      shard: this.#batch!.shard,
      ts: this.#batch!.ts,
      format: Format.JSON_STREAM,
      size: {
        bytes: written,
        rows: data.length,
      },
      data: readable,
    };
  }
}
