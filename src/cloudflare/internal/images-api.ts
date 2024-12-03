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
  public constructor(private readonly bindingsResponse: Response) {}

  public contentType(): string {
    const contentType = this.bindingsResponse.headers.get('content-type');
    if (!contentType) {
      throw new ImagesErrorImpl(
        'IMAGES_TRANSFORM_ERROR 9523: No content-type on bindings response',
        9523
      );
    }

    return contentType;
  }

  public image(): ReadableStream<Uint8Array> {
    return this.bindingsResponse.body || new ReadableStream();
  }

  public response(): Response {
    return new Response(this.image(), {
      headers: {
        'content-type': this.contentType(),
      },
    });
  }
}

class DrawTransformer {
  public constructor(
    public readonly child: ImageTransformerImpl,
    public readonly options: ImageDrawOptions
  ) {}
}

class ImageTransformerImpl implements ImageTransformer {
  private transforms: (ImageTransform | DrawTransformer)[];
  private consumed: boolean;

  public constructor(
    private readonly fetcher: Fetcher,
    private readonly stream: ReadableStream<Uint8Array>
  ) {
    this.transforms = [];
    this.consumed = false;
  }

  public transform(transform: ImageTransform): this {
    this.transforms.push(transform);
    return this;
  }

  public draw(
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

  public async output(
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
  public constructor(private readonly fetcher: Fetcher) {}

  public async info(
    stream: ReadableStream<Uint8Array>
  ): Promise<ImageInfoResponse> {
    const body = new StreamableFormData();
    body.append('image', stream, { type: 'file' });

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

  public input(stream: ReadableStream<Uint8Array>): ImageTransformer {
    return new ImageTransformerImpl(this.fetcher, stream);
  }
}

class ImagesErrorImpl extends Error implements ImagesError {
  public constructor(
    message: string,
    public readonly code: number
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

  public constructor() {
    this.entries = [];

    this.boundary = '--------------------------';
    for (let i = 0; i < 24; i++) {
      this.boundary += Math.floor(Math.random() * 10).toString(16);
    }
  }

  public append(
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

  public contentType(): string {
    return `multipart/form-data; boundary=${this.boundary}`;
  }

  public stream(): ReadableStream {
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
