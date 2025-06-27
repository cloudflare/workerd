import base64 from 'cloudflare-internal:base64';

function base64Error(cause: unknown): Error {
  // @ts-expect-error: Error.isError seems to be missing from types?
  const isError: (_: unknown) => boolean = Error.isError; // eslint-disable-line @typescript-eslint/no-unsafe-assignment
  if (isError(cause)) {
    const e = cause as Error;
    return new Error(`base64 error: ${e.message}`, { cause });
  } else {
    return new Error('unknown base64 error');
  }
}

function toBase64(input: Uint8Array): Uint8Array {
  return new Uint8Array(base64.encodeArray(input));
}

function fromBase64(input: Uint8Array): Uint8Array {
  return new Uint8Array(base64.decodeArray(input));
}

function combineArrays(a: Uint8Array, b: Uint8Array): Uint8Array {
  const combined = new Uint8Array(a.length + b.length);
  combined.set(a, 0);
  combined.set(b, a.length);
  return combined;
}

function buildChunkFromLeftover(
  leftover: Uint8Array,
  chunk: Uint8Array,
  minimumChunkSize: number
): { firstChunk: Uint8Array; remainder: Uint8Array } {
  const offset = minimumChunkSize - leftover.length;
  const firstChunk = new Uint8Array(minimumChunkSize);
  firstChunk.set(leftover, 0);
  firstChunk.set(chunk.subarray(0, offset), leftover.length);
  return { firstChunk, remainder: chunk.subarray(offset) };
}

function getProcessableChunk(
  chunk: Uint8Array,
  segmentSize: number
): { processable: Uint8Array; leftover: Uint8Array | null } {
  const processableLength =
    Math.trunc(chunk.length / segmentSize) * segmentSize;

  const processable =
    processableLength > 0
      ? chunk.subarray(0, processableLength)
      : new Uint8Array();
  const leftover =
    processableLength !== chunk.length
      ? chunk.subarray(processableLength)
      : null;

  return { processable, leftover };
}

const PADDING_CHAR_CODE = '='.charCodeAt(0);
function isPaddedBase64Chunk(chunk: Uint8Array): boolean {
  return chunk.length > 0 && chunk[chunk.length - 1] === PADDING_CHAR_CODE;
}

export function createBase64EncoderTransformStream(
  maxEncodeChunkSize: number = 32 * 1024 + 1
): TransformStream<Uint8Array, Uint8Array> {
  let leftover: Uint8Array | null = null;

  if (maxEncodeChunkSize % 3 != 0) {
    // Try to minimise padding
    throw new Error('maxChunkSize must be a multiple of 3');
  }

  return new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) {
      if (leftover != null) {
        const requiredBytes = 3 - leftover.length;

        if (chunk.length < requiredBytes) {
          // We don't have enough bytes in the chunk to encode, update leftovers
          leftover = combineArrays(leftover, chunk);
          // encode when we get more chars
          return;
        }

        const { firstChunk, remainder } = buildChunkFromLeftover(
          leftover,
          chunk,
          3
        );
        controller.enqueue(toBase64(firstChunk));
        leftover = null;
        chunk = remainder;
      }

      while (chunk.length >= maxEncodeChunkSize) {
        controller.enqueue(toBase64(chunk.subarray(0, maxEncodeChunkSize)));
        chunk = chunk.subarray(maxEncodeChunkSize);
      }

      // Encode what's encodable in what's left of the chunk
      const { processable, leftover: nextLeftover } = getProcessableChunk(
        chunk,
        3
      );

      if (processable.length > 0) {
        controller.enqueue(toBase64(processable));
      }

      leftover = nextLeftover;
    },

    flush(controller) {
      if (leftover != null) {
        controller.enqueue(toBase64(leftover));
      }
    },
  });
}

export function createBase64DecoderTransformStream(
  maxChunkSize: number = 32 * 1024
): TransformStream<Uint8Array, Uint8Array> {
  if (maxChunkSize % 4 !== 0 || maxChunkSize <= 0) {
    throw new Error('maxChunkSize must be a positive multiple of 4.');
  }

  let leftover: Uint8Array | null = null;
  let paddingSeen = false;

  return new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) {
      try {
        // If we see a chunk with padding, we aren't allowed to see any more data, as padding is allowed only at the end
        if (paddingSeen && chunk.length > 0) {
          throw new Error('Padding already seen, no further chunks allowed');
        } else if (isPaddedBase64Chunk(chunk)) {
          paddingSeen = true;
        }

        if (leftover != null) {
          // We have leftovers - decode 4 bytes consisting of the leftovers + part of the chunk
          const requiredBytes = 4 - leftover.length;

          if (chunk.length < requiredBytes) {
            // We don't have enough bytes in the chunk to decode, update leftovers
            leftover = combineArrays(leftover, chunk);
            // Decode when we get more chars
            return;
          }

          const { firstChunk, remainder } = buildChunkFromLeftover(
            leftover,
            chunk,
            4
          );
          controller.enqueue(fromBase64(firstChunk));
          leftover = null;
          chunk = remainder;
        }

        while (chunk.length >= maxChunkSize) {
          controller.enqueue(fromBase64(chunk.subarray(0, maxChunkSize)));
          chunk = chunk.subarray(maxChunkSize);
        }

        // Decode what's decodable in what's left of the chunk
        const { processable, leftover: nextLeftover } = getProcessableChunk(
          chunk,
          4
        );

        if (processable.length > 0) {
          controller.enqueue(fromBase64(processable));
        }

        leftover = nextLeftover;
      } catch (e) {
        controller.error(base64Error(e));
      }
    },

    flush(controller) {
      if (leftover != null) {
        // We have leftovers, but they aren't decodable
        controller.error(
          base64Error(new Error('Bytes left over when flushing controller'))
        );
      }
    },
  });
}
