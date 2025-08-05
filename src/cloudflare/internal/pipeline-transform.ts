// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import entrypoints from 'cloudflare-internal:workers';

/**
 * Reads a stream line by line, yielding each line as it becomes available.
 *
 * This function consumes a ReadableStream of Uint8Array chunks (binary data),
 * converts it to text using TextDecoderStream, and yields each line
 * encountered. Lines are delimited by newline characters ('\n'). The final
 * line is yielded even if it doesn't end with a newline.
 *
 * @param stream - A ReadableStream containing binary data to be decoded as text
 * @returns An AsyncGenerator that yields each line from the stream
 */
async function* readLines(
  stream: ReadableStream<Uint8Array>
): AsyncGenerator<string> {
  const textStream = stream.pipeThrough(new TextDecoderStream());
  const reader = textStream.getReader();

  let buffer = '';

  try {
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    while (true) {
      const { done, value } = await reader.read();

      // Add any new content to the buffer
      if (value) buffer += value;

      // Process complete lines
      let lineEndIndex = buffer.indexOf('\n');
      while (lineEndIndex >= 0) {
        yield buffer.substring(0, lineEndIndex);
        buffer = buffer.substring(lineEndIndex + 1);
        lineEndIndex = buffer.indexOf('\n');
      }

      // If we're done and have processed all complete lines,
      // yield any remaining content and exit
      if (done) {
        if (buffer.length > 0) {
          yield buffer;
        }
        break;
      }
    }
  } finally {
    reader.releaseLock();
  }
}

type Batch = {
  id: string; // unique identifier for the batch
  shard: string; // assigned shard
  ts: number; // creation timestamp of the batch

  format: FormatType;
  size: {
    bytes: number;
    rows: number;
  };
  data: unknown;
};

const Format = {
  JSON_STREAM: 'json_stream' as const, // jsonl
};
type FormatType = (typeof Format)[keyof typeof Format];

type JsonStream = Batch & {
  format: typeof Format.JSON_STREAM;
  data: ReadableStream<Uint8Array>;
};

type PipelineBatchMetadata = {
  pipelineId: string;
  pipelineName: string;
};

type PipelineRecord = Record<string, unknown>;

export class PipelineTransformImpl<
  I extends PipelineRecord,
  O extends PipelineRecord,
> extends entrypoints.WorkerEntrypoint {
  #batch?: Batch;
  #initalized: boolean = false;

  // stub overridden on the subclass
  // eslint-disable-next-line @typescript-eslint/require-await
  async run(_records: I[], _metadata: PipelineBatchMetadata): Promise<O[]> {
    throw new Error('should be implemented by parent');
  }

  // called by the dispatcher to validate that run is properly implemented by the subclass
  // @ts-expect-error This is OK. We use this method in tests.
  // eslint-disable-next-line no-restricted-syntax
  private async _ping(): Promise<void> {
    // making sure the function was overridden by an implementing subclass
    if (this.run !== PipelineTransformImpl.prototype.run) {
      return Promise.resolve();
    } else {
      return Promise.reject(
        new Error(
          'the run method must be overridden by the PipelineTransformationEntrypoint subclass'
        )
      );
    }
  }

  // called by the dispatcher which then calls the subclass methods
  // the reason this is typescript private and not javascript private is that this must be
  // able to be called by the dispatcher but should not be called by the class implementer
  // @ts-expect-error This is OK. We use this method in tests.
  // eslint-disable-next-line no-restricted-syntax
  private async _run(
    batch: Batch,
    metadata: PipelineBatchMetadata
  ): Promise<JsonStream> {
    if (this.#initalized) {
      throw new Error('pipeline entrypoint has already been initialized');
    }

    this.#batch = batch;
    this.#initalized = true;

    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    if (this.#batch.format === Format.JSON_STREAM) {
      const records: I[] = await this.#readJsonStream();
      const transformed = await this.run(records, metadata);
      return this.#sendJson(transformed);
    } else {
      throw new Error(
        'PipelineTransformationEntrypoint run supports only the JSON_STREAM batch format'
      );
    }
  }

  async #readJsonStream(): Promise<I[]> {
    if (this.#batch?.format !== Format.JSON_STREAM) {
      throw new Error(`expected JSON_STREAM not ${this.#batch?.format}`);
    }

    const batch = this.#batch.data as ReadableStream<Uint8Array>;

    const data: I[] = [];
    for await (const line of readLines(batch)) {
      if (line.trim().length > 0) {
        // guard against empty lines
        data.push(JSON.parse(line) as I);
      }
    }

    return data;
  }

  #sendJson(records: O[]): JsonStream {
    if (!(records instanceof Array)) {
      throw new Error('transformations must return an array of PipelineRecord');
    }

    let written = 0;
    const encoder = new TextEncoder();
    const readable = new ReadableStream<Uint8Array>({
      start(controller): void {
        for (const record of records) {
          const encoded = encoder.encode(`${JSON.stringify(record)}\n`);
          written += encoded.length;
          controller.enqueue(encoded);
        }

        controller.close();
      },
    });

    if (!this.#batch) {
      throw new Error('Batch should have been defined. Assertion error.');
    }

    return {
      id: this.#batch.id,
      shard: this.#batch.shard,
      ts: this.#batch.ts,
      format: Format.JSON_STREAM,
      size: {
        bytes: written,
        rows: records.length,
      },
      data: readable,
    };
  }
}
