// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { StreamableFormData } from 'cloudflare-internal:streaming-forms';
import {
  createBase64DecoderTransformStream,
  createBase64EncoderTransformStream,
} from 'cloudflare-internal:streaming-base64';

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
    const stream = this.bindingsResponse.body || new Blob().stream();

    return options?.encoding === 'base64'
      ? stream.pipeThrough(createBase64EncoderTransformStream())
      : stream;
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

    const decodedStream =
      options?.encoding === 'base64'
        ? stream.pipeThrough(createBase64DecoderTransformStream())
        : stream;

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
    const decodedStream =
      options?.encoding === 'base64'
        ? stream.pipeThrough(createBase64DecoderTransformStream())
        : stream;

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
