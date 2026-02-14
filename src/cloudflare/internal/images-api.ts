// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { StreamableFormData } from 'cloudflare-internal:streaming-forms';
import {
  createBase64DecoderTransformStream,
  createBase64EncoderTransformStream,
} from 'cloudflare-internal:streaming-base64';
import { withSpan, type Span } from 'cloudflare-internal:tracing-helpers';

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
  readonly #bindingsResponse: Response;

  constructor(bindingsResponse: Response) {
    this.#bindingsResponse = bindingsResponse;
  }

  contentType(): string {
    const contentType = this.#bindingsResponse.headers.get('content-type');
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
    const stream = this.#bindingsResponse.body || new Blob().stream();

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
  readonly child: ImageTransformerImpl;
  readonly options: ImageDrawOptions;
  constructor(child: ImageTransformerImpl, options: ImageDrawOptions) {
    this.child = child;
    this.options = options;
  }
}

class ImageTransformerImpl implements ImageTransformer {
  readonly #fetcher: Fetcher;
  readonly #stream: ReadableStream<Uint8Array>;

  #transforms: (ImageTransform | DrawTransformer)[];
  #consumed: boolean;

  constructor(fetcher: Fetcher, stream: ReadableStream<Uint8Array>) {
    this.#fetcher = fetcher;
    this.#stream = stream;
    this.#transforms = [];
    this.#consumed = false;
  }

  transform(transform: ImageTransform): this {
    this.#transforms.push(transform);
    return this;
  }

  draw(
    image: ReadableStream<Uint8Array> | ImageTransformer,
    options: ImageDrawOptions = {}
  ): this {
    if (isTransformer(image)) {
      image.#consume();
      this.#transforms.push(new DrawTransformer(image, options));
    } else {
      this.#transforms.push(
        new DrawTransformer(
          new ImageTransformerImpl(
            this.#fetcher,
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
    return await withSpan('images_output', async (span) => {
      span.setAttribute('cloudflare.binding.type', 'Images');
      const formData = new StreamableFormData();

      this.#consume();
      formData.append('image', this.#stream, { type: 'file' });

      this.#serializeTransforms(formData, span);

      span.setAttribute('cloudflare.images.options.format', options.format);
      formData.append('output_format', options.format);

      if (options.quality !== undefined) {
        span.setAttribute('cloudflare.images.options.quality', options.quality);
        formData.append('output_quality', options.quality.toString());
      }

      if (options.background !== undefined) {
        span.setAttribute(
          'cloudflare.images.options.background',
          options.background
        );
        formData.append('background', options.background);
      }

      if (options.anim !== undefined) {
        span.setAttribute('cloudflare.images.options.anim', options.anim);
        formData.append('anim', options.anim.toString());
      }

      const response = await this.#fetcher.fetch(
        'https://js.images.cloudflare.com/transform',
        {
          method: 'POST',
          headers: {
            'content-type': formData.contentType(),
          },
          body: formData.stream(),
        }
      );

      await throwErrorIfErrorResponse('TRANSFORM', response, span);

      return new TransformationResultImpl(response);
    });
  }

  #consume(): void {
    if (this.#consumed) {
      throw new ImagesErrorImpl(
        'IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() or draw a transformer once',
        9525
      );
    }

    this.#consumed = true;
  }

  #serializeTransforms(formData: StreamableFormData, span: Span): void {
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
          const drawImageIndex = appendDrawImage(transform.child.#stream);

          // Tell the backend to run any transforms (possibly involving more draws)
          // required to build this child
          walkTransforms(drawImageIndex, transform.child.#transforms);

          // Draw the child image on to the canvas
          transforms.push({
            drawImageIndex: drawImageIndex,
            targetImageIndex: targetImageIndex,
            ...transform.options,
          });
        }
      }
    }

    walkTransforms(0, this.#transforms);

    // The transforms are a set of operations which are applied to the image in order.
    // Attaching an attribute as JSON is a little odd, but I'm not sure if there is
    // a better way to do this.
    if (transforms.length > 0) {
      span.setAttribute(
        'cloudflare.images.options.transforms',
        JSON.stringify(transforms)
      );
    }
    formData.append('transforms', JSON.stringify(transforms));
  }
}

function isTransformer(input: unknown): input is ImageTransformerImpl {
  return input instanceof ImageTransformerImpl;
}

function isDrawTransformer(input: unknown): input is DrawTransformer {
  return input instanceof DrawTransformer;
}

interface ServiceEntrypointStub {
  details(imageId: string): Promise<ImageMetadata | null>;
  image(imageId: string): Promise<ReadableStream<Uint8Array> | null>;
  upload(
    image: ReadableStream<Uint8Array> | ArrayBuffer,
    options?: ImageUploadOptions
  ): Promise<ImageMetadata>;
  update(imageId: string, options: ImageUpdateOptions): Promise<ImageMetadata>;
  delete(imageId: string): Promise<boolean>;
  list(options?: ImageListOptions): Promise<ImageList>;
}

class HostedImagesBindingImpl implements HostedImagesBinding {
  readonly #fetcher: ServiceEntrypointStub;

  constructor(fetcher: ServiceEntrypointStub) {
    this.#fetcher = fetcher;
  }

  async details(imageId: string): Promise<ImageMetadata | null> {
    return this.#fetcher.details(imageId);
  }

  async image(imageId: string): Promise<ReadableStream<Uint8Array> | null> {
    return this.#fetcher.image(imageId);
  }

  async upload(
    image: ReadableStream<Uint8Array> | ArrayBuffer,
    options?: ImageUploadOptions
  ): Promise<ImageMetadata> {
    return this.#fetcher.upload(image, options);
  }

  async update(
    imageId: string,
    options: ImageUpdateOptions
  ): Promise<ImageMetadata> {
    return this.#fetcher.update(imageId, options);
  }

  async delete(imageId: string): Promise<boolean> {
    return this.#fetcher.delete(imageId);
  }

  async list(options?: ImageListOptions): Promise<ImageList> {
    return this.#fetcher.list(options || {});
  }
}

class ImagesBindingImpl implements ImagesBinding {
  readonly #fetcher: Fetcher & ServiceEntrypointStub;
  readonly #hosted: HostedImagesBinding;

  constructor(fetcher: Fetcher & ServiceEntrypointStub) {
    this.#fetcher = fetcher;
    this.#hosted = new HostedImagesBindingImpl(fetcher);
  }

  get hosted(): HostedImagesBinding {
    return this.#hosted;
  }

  async info(
    stream: ReadableStream<Uint8Array>,
    options?: ImageInputOptions
  ): Promise<ImageInfoResponse> {
    return await withSpan('images_info', async (span) => {
      span.setAttribute('cloudflare.binding.type', 'Images');
      const body = new StreamableFormData();

      span.setAttribute(
        'cloudflare.images.options.encoding',
        options?.encoding ?? 'base64'
      );
      const decodedStream =
        options?.encoding === 'base64'
          ? stream.pipeThrough(createBase64DecoderTransformStream())
          : stream;

      body.append('image', decodedStream, { type: 'file' });

      const response = await this.#fetcher.fetch(
        'https://js.images.cloudflare.com/info',
        {
          method: 'POST',
          headers: {
            'content-type': body.contentType(),
          },
          body: body.stream(),
        }
      );

      await throwErrorIfErrorResponse('INFO', response, span);

      const r = (await response.json()) as RawInfoResponse;

      span.setAttribute('cloudflare.images.result.format', r.format);

      if ('file_size' in r) {
        const ret = {
          fileSize: r.file_size,
          width: r.width,
          height: r.height,
          format: r.format,
        };
        span.setAttribute('cloudflare.images.result.file_size', ret.fileSize);
        span.setAttribute('cloudflare.images.result.width', ret.width);
        span.setAttribute('cloudflare.images.result.height', ret.height);
        return ret;
      }

      return r;
    });
  }

  input(
    stream: ReadableStream<Uint8Array>,
    options?: ImageInputOptions
  ): ImageTransformer {
    const decodedStream =
      options?.encoding === 'base64'
        ? stream.pipeThrough(createBase64DecoderTransformStream())
        : stream;

    return new ImageTransformerImpl(this.#fetcher, decodedStream);
  }
}

class ImagesErrorImpl extends Error implements ImagesError {
  readonly code: number;
  constructor(message: string, code: number) {
    super(message);
    this.code = code;
  }
}

async function throwErrorIfErrorResponse(
  operation: string,
  response: Response,
  span: Span
): Promise<void> {
  const statusHeader = response.headers.get('cf-images-binding') || '';

  const match = /err=(\d+)/.exec(statusHeader);

  if (match && match[1]) {
    const errorMessage = await response.text();
    span.setAttribute('cloudflare.images.error.code', match[1]);
    span.setAttribute('error.type', errorMessage);
    throw new ImagesErrorImpl(
      `IMAGES_${operation}_${errorMessage}`.trim(),
      Number.parseInt(match[1])
    );
  }

  if (response.status > 399) {
    const errorMessage = await response.text();
    span.setAttribute('cloudflare.images.error.code', '9523');
    span.setAttribute('error.type', errorMessage);
    throw new ImagesErrorImpl(
      `Unexpected error response ${response.status}: ${errorMessage.trim()}`,
      9523
    );
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): ImagesBinding {
  return new ImagesBindingImpl(env.fetcher as Fetcher & ServiceEntrypointStub);
}
