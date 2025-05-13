// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

type Fetcher = {
  fetch: typeof fetch;
};

type TargetedTransform = ImageTransform & {
  imageIndex: number;
};

// Draw image drawImageIndex on image targetImageIndex
type DrawCommand = ImageDrawOptions & {
  drawImageIndex: number;
  targetImageIndex: number;
};

type RawInfoResponse =
  | { format: 'image/svg+xml' }
  | {
      format: string;
      file_size: number;
      width: number;
      height: number;
    };

class TransformationResultImpl implements ImageTransformationResult {
  constructor(private readonly bindingsResponse: Response) {}

  contentType(): string {
    const contentType = this.bindingsResponse.headers.get('content-type');
    if (!contentType) {
      throw new ImagesErrorImpl(
        'IMAGES_TRANSFORM_ERROR 9523: No content-type on bindings response',
        9523
      );
    }

    return contentType;
  }

  image(
    options?: ImageTransformationOutputOptions
  ): ReadableStream<Uint8Array> {
    let stream = this.bindingsResponse.body || new ReadableStream();

    if (options?.encoding === 'base64') {
      stream = stream.pipeThrough(createBase64EncoderTransformStream());
    }

    return stream;
  }

  response(): Response {
    return new Response(this.image(), {
      headers: {
        'content-type': this.contentType(),
      },
    });
  }
}

class DrawTransformer {
  constructor(
    readonly child: ImageTransformerImpl,
    readonly options: ImageDrawOptions
  ) {}
}

class ImageTransformerImpl implements ImageTransformer {
  private transforms: (ImageTransform | DrawTransformer)[];
  private consumed: boolean;

  constructor(
    private readonly fetcher: Fetcher,
    private readonly stream: ReadableStream<Uint8Array>
  ) {
    this.transforms = [];
    this.consumed = false;
  }

  transform(transform: ImageTransform): this {
    this.transforms.push(transform);
    return this;
  }

  draw(
    image: ReadableStream<Uint8Array> | ImageTransformer,
    options: ImageDrawOptions = {}
  ): this {
    if (isTransformer(image)) {
      image.consume();
      this.transforms.push(new DrawTransformer(image, options));
    } else {
      this.transforms.push(
        new DrawTransformer(
          new ImageTransformerImpl(
            this.fetcher,
            image as ReadableStream<Uint8Array>
          ),
          options
        )
      );
    }

    return this;
  }

  async output(
    options: ImageOutputOptions
  ): Promise<ImageTransformationResult> {
    const formData = new StreamableFormData();

    this.consume();
    formData.append('image', this.stream, { type: 'file' });

    this.serializeTransforms(formData);

    formData.append('output_format', options.format);
    if (options.quality !== undefined) {
      formData.append('output_quality', options.quality.toString());
    }

    if (options.background !== undefined) {
      formData.append('background', options.background);
    }

    const response = await this.fetcher.fetch(
      'https://js.images.cloudflare.com/transform',
      {
        method: 'POST',
        headers: {
          'content-type': formData.contentType(),
        },
        body: formData.stream(),
      }
    );

    await throwErrorIfErrorResponse('TRANSFORM', response);

    return new TransformationResultImpl(response);
  }

  private consume(): void {
    if (this.consumed) {
      throw new ImagesErrorImpl(
        'IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() or draw a transformer once',
        9525
      );
    }

    this.consumed = true;
  }

  private serializeTransforms(formData: StreamableFormData): void {
    const transforms: (TargetedTransform | DrawCommand)[] = [];

    // image 0 is the canvas, so the first draw_image has index 1
    let drawImageIndex = 1;
    function appendDrawImage(stream: ReadableStream): number {
      formData.append('draw_image', stream, { type: 'file' });
      return drawImageIndex++;
    }

    function walkTransforms(
      targetImageIndex: number,
      imageTransforms: (ImageTransform | DrawTransformer)[]
    ): void {
      for (const transform of imageTransforms) {
        if (!isDrawTransformer(transform)) {
          // Simple transformation - we just have to tell the backend to run it
          // against this image
          transforms.push({
            imageIndex: targetImageIndex,
            ...transform,
          });
        } else {
          // Drawn child image
          // Set the input for the drawn image on the form
          const drawImageIndex = appendDrawImage(transform.child.stream);

          // Tell the backend to run any transforms (possibly involving more draws)
          // required to build this child
          walkTransforms(drawImageIndex, transform.child.transforms);

          // Draw the child image on to the canvas
          transforms.push({
            drawImageIndex: drawImageIndex,
            targetImageIndex: targetImageIndex,
            ...transform.options,
          });
        }
      }
    }

    walkTransforms(0, this.transforms);
    formData.append('transforms', JSON.stringify(transforms));
  }
}

function isTransformer(input: unknown): input is ImageTransformerImpl {
  return input instanceof ImageTransformerImpl;
}

function isDrawTransformer(input: unknown): input is DrawTransformer {
  return input instanceof DrawTransformer;
}

class ImagesBindingImpl implements ImagesBinding {
  constructor(private readonly fetcher: Fetcher) {}

  async info(
    stream: ReadableStream<Uint8Array>,
    options?: ImageInputOptions
  ): Promise<ImageInfoResponse> {
    const body = new StreamableFormData();

    let decodedStream = stream;
    if (options?.encoding === 'base64') {
      decodedStream = stream.pipeThrough(createBase64DecoderTransformStream());
    }

    body.append('image', decodedStream, { type: 'file' });

    const response = await this.fetcher.fetch(
      'https://js.images.cloudflare.com/info',
      {
        method: 'POST',
        headers: {
          'content-type': body.contentType(),
        },
        body: body.stream(),
      }
    );

    await throwErrorIfErrorResponse('INFO', response);

    const r = (await response.json()) as RawInfoResponse;

    if ('file_size' in r) {
      return {
        fileSize: r.file_size,
        width: r.width,
        height: r.height,
        format: r.format,
      };
    }

    return r;
  }

  input(
    stream: ReadableStream<Uint8Array>,
    options?: ImageInputOptions
  ): ImageTransformer {
    let decodedStream = stream;

    if (options?.encoding === 'base64') {
      decodedStream = stream.pipeThrough(createBase64DecoderTransformStream());
    }

    return new ImageTransformerImpl(this.fetcher, decodedStream);
  }
}

class ImagesErrorImpl extends Error implements ImagesError {
  constructor(
    message: string,
    readonly code: number
  ) {
    super(message);
  }
}

async function throwErrorIfErrorResponse(
  operation: string,
  response: Response
): Promise<void> {
  const statusHeader = response.headers.get('cf-images-binding') || '';

  const match = /err=(\d+)/.exec(statusHeader);

  if (match && match[1]) {
    throw new ImagesErrorImpl(
      `IMAGES_${operation}_${await response.text()}`.trim(),
      Number.parseInt(match[1])
    );
  }

  if (response.status > 399) {
    throw new ImagesErrorImpl(
      `Unexpected error response ${response.status}: ${(
        await response.text()
      ).trim()}`,
      9523
    );
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): ImagesBinding {
  return new ImagesBindingImpl(env.fetcher);
}

function chainStreams<T>(streams: ReadableStream<T>[]): ReadableStream<T> {
  const outputStream = new ReadableStream<T>({
    async start(controller): Promise<void> {
      for (const stream of streams) {
        const reader = stream.getReader();

        try {
          // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            if (value !== undefined) controller.enqueue(value);
          }
        } finally {
          reader.releaseLock();
        }
      }

      controller.close();
    },
  });

  return outputStream;
}

function concatUint8Arrays(a: Uint8Array, b: Uint8Array): Uint8Array {
  const result = new Uint8Array(a.length + b.length);
  result.set(a, 0);
  result.set(b, a.length);
  return result;
}

class Base64Error extends Error {
  public constructor(cause: unknown) {
    if (cause instanceof Error) {
      super(`base64 error: ${cause.message}`, { cause });
    } else {
      super('unknown base64 error');
    }
  }
}

function createBase64EncoderTransformStream(
  maxEncodeChunkSize: number = 32 * 1024 + 1
): TransformStream<Uint8Array, Uint8Array> {
  if (maxEncodeChunkSize % 3 != 0) {
    // Ensures that chunks don't require padding
    throw new Error('maxChunkSize must be a multiple of 3');
  }

  let buffer: Uint8Array | null = null;
  const asciiEncoder = new TextEncoder();

  const toBase64: (buf: Uint8Array) => Uint8Array = (buf) => {
    const binaryString = String.fromCharCode.apply(null, Array.from(buf));

    const base64String = btoa(binaryString);
    const base64Bytes = asciiEncoder.encode(base64String);

    return base64Bytes;
  };

  return new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller): void {
      const currentData = buffer ? concatUint8Arrays(buffer, chunk) : chunk;
      buffer = null;

      let offset = 0;

      while (currentData.length - offset >= maxEncodeChunkSize) {
        const sliceToEnd = offset + maxEncodeChunkSize;
        const processChunk = currentData.slice(offset, sliceToEnd);
        offset = sliceToEnd;

        try {
          controller.enqueue(toBase64(processChunk));
        } catch (error) {
          controller.error(new Base64Error(error));
          buffer = null;
          return;
        }
      }

      buffer = offset < currentData.length ? currentData.slice(offset) : null;
    },

    flush(controller): void {
      if (buffer && buffer.length > 0) {
        try {
          controller.enqueue(toBase64(buffer));
        } catch (error) {
          const errMsg = error instanceof Error ? error.message : String(error);
          controller.error(new Error(`base64 encoding error: ${errMsg}`));
        }
      }
      buffer = null;
    },
  });
}

function createBase64DecoderTransformStream(
  maxChunkSize: number = 32 * 1024
): TransformStream<Uint8Array, Uint8Array> {
  if (maxChunkSize % 4 !== 0 || maxChunkSize <= 0) {
    throw new Error('maxChunkSize must be a positive multiple of 4.');
  }

  let base64Buffer: Uint8Array | null = null;
  const asciiDecoder = new TextDecoder('ascii');

  const decodeAndEnqueueSegment = (
    base64SegmentBytes: Uint8Array,
    controller: TransformStreamDefaultController<Uint8Array>
  ): boolean => {
    try {
      const base64SegmentString = asciiDecoder.decode(base64SegmentBytes);

      const binaryString = atob(base64SegmentString);

      if (binaryString.length > 0) {
        const decodedBytes = Uint8Array.from(binaryString, (c) =>
          c.charCodeAt(0)
        );

        controller.enqueue(decodedBytes);
      }
      return true;
    } catch (error) {
      controller.error(new Base64Error(error));
      return false;
    }
  };

  return new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller): void {
      base64Buffer = base64Buffer
        ? concatUint8Arrays(base64Buffer, chunk)
        : chunk;

      while (base64Buffer && base64Buffer.length >= maxChunkSize) {
        const base64SegmentBytes = base64Buffer.slice(0, maxChunkSize);
        base64Buffer =
          base64Buffer.length > maxChunkSize
            ? base64Buffer.slice(maxChunkSize)
            : null;

        if (!decodeAndEnqueueSegment(base64SegmentBytes, controller)) {
          base64Buffer = null;
          return;
        }
      }

      const remainingProcessableLength =
        Math.floor(base64Buffer?.length ?? 0 / 4) * 4;
      if (remainingProcessableLength > 0) {
        if (base64Buffer) {
          const base64SegmentBytes = base64Buffer.slice(
            0,
            remainingProcessableLength
          );
          base64Buffer =
            base64Buffer.length > remainingProcessableLength
              ? base64Buffer.slice(remainingProcessableLength)
              : null;

          if (!decodeAndEnqueueSegment(base64SegmentBytes, controller)) {
            base64Buffer = null;
            return;
          }
        }
      }
    },

    flush(controller): void {
      if (base64Buffer && base64Buffer.length > 0) {
        if (!decodeAndEnqueueSegment(base64Buffer, controller)) {
          // Error already handled within the helper
        }
      }
      base64Buffer = null;
    },
  });
}

const CRLF = '\r\n';

function isReadableStream(obj: unknown): obj is ReadableStream {
  return !!(
    obj &&
    typeof obj === 'object' &&
    'getReader' in obj &&
    typeof obj.getReader === 'function'
  );
}

type EntryOptions = { type: 'file' | 'string' };
class StreamableFormData {
  private entries: {
    field: string;
    value: ReadableStream;
    options: EntryOptions;
  }[];
  private boundary: string;

  constructor() {
    this.entries = [];

    this.boundary = '--------------------------';
    for (let i = 0; i < 24; i++) {
      this.boundary += Math.floor(Math.random() * 10).toString(16);
    }
  }

  append(
    field: string,
    value: ReadableStream | string,
    options?: EntryOptions
  ): void {
    let valueStream: ReadableStream;
    if (isReadableStream(value)) {
      valueStream = value;
    } else {
      valueStream = new Blob([value]).stream();
    }

    this.entries.push({
      field,
      value: valueStream,
      options: options || { type: 'string' },
    });
  }

  private multipartBoundary(): ReadableStream {
    return new Blob(['--', this.boundary, CRLF]).stream();
  }

  private multipartHeader(
    name: string,
    type: 'file' | 'string'
  ): ReadableStream {
    let filenamePart;

    if (type === 'file') {
      filenamePart = `; filename="${name}"`;
    } else {
      filenamePart = '';
    }

    return new Blob([
      `content-disposition: form-data; name="${name}"${filenamePart}`,
      CRLF,
      CRLF,
    ]).stream();
  }

  private multipartBody(stream: ReadableStream): ReadableStream {
    return chainStreams([stream, new Blob([CRLF]).stream()]);
  }

  private multipartFooter(): ReadableStream {
    return new Blob(['--', this.boundary, '--', CRLF]).stream();
  }

  contentType(): string {
    return `multipart/form-data; boundary=${this.boundary}`;
  }

  stream(): ReadableStream {
    const streams: ReadableStream[] = [this.multipartBoundary()];

    const valueStreams = [];
    for (const { field, value, options } of this.entries) {
      valueStreams.push(this.multipartHeader(field, options.type));
      valueStreams.push(this.multipartBody(value));
      valueStreams.push(this.multipartBoundary());
    }

    if (valueStreams.length) {
      // Remove last boundary as we want a footer instead
      valueStreams.pop();
    }

    streams.push(...valueStreams);

    streams.push(this.multipartFooter());

    return chainStreams(streams);
  }
}
