// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import entrypoints from 'cloudflare-internal:workers'

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
async function* _readLines(
  stream: ReadableStream<Uint8Array>,
): AsyncGenerator<string> {
  // @ts-expect-error TS2345 TODO(soon): Fix this.
  const textStream = stream.pipeThrough(new TextDecoderStream())
  const reader = textStream.getReader()

  let buffer = ''

  try {
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    while (true) {
      const { done, value } = await reader.read()

      // Add any new content to the buffer
      if (value) buffer += value

      // Process complete lines
      let lineEndIndex = buffer.indexOf('\n')
      while (lineEndIndex >= 0) {
        yield buffer.substring(0, lineEndIndex)
        buffer = buffer.substring(lineEndIndex + 1)
        lineEndIndex = buffer.indexOf('\n')
      }

      // If we're done and have processed all complete lines,
      // yield any remaining content and exit
      if (done) {
        if (buffer.length > 0) {
          yield buffer
        }
        break
      }
    }
  } finally {
    reader.releaseLock()
  }
}

type Batch = {
  id: string // unique identifier for the batch
  shard: string // assigned shard
  ts: number // creation timestamp of the batch

  format: FormatType
  size: {
    bytes: number
    rows: number
  }
  data: unknown
}

const Format = {
  JSON_STREAM: 'json_stream' as const, // jsonl
}
type FormatType = (typeof Format)[keyof typeof Format]

type JsonStream = Batch & {
  format: typeof Format.JSON_STREAM
  data: ReadableStream<Uint8Array>
}

type PipelineBatchMetadata = {
  pipelineId: string
  pipelineName: string
}

type PipelineRecord = Record<string, unknown>

export class PipelineTransformImpl<
  I extends PipelineRecord,
  O extends PipelineRecord,
> extends entrypoints.WorkerEntrypoint {
  // stub overridden on the subclass
  // eslint-disable-next-line @typescript-eslint/require-await
  async run(_records: I[], _metadata: PipelineBatchMetadata): Promise<O[]> {
    throw new Error('should be implemented by parent')
  }
}
