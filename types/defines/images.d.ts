// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

type ImageInfoResponse =
  | { format: 'image/svg+xml' }
  | {
      format: string;
      fileSize: number;
      width: number;
      height: number;
    };

type ImageTransform = {
  fit?: 'scale-down' | 'contain' | 'pad' | 'squeeze' | 'cover' | 'crop';
  gravity?:
    | 'left'
    | 'right'
    | 'top'
    | 'bottom'
    | 'center'
    | 'auto'
    | 'entropy'
    | 'face'
    | {
        x?: number;
        y?: number;
        mode: 'remainder' | 'box-center';
      };
  trim?: {
    top?: number;
    bottom?: number;
    left?: number;
    right?: number;
    width?: number;
    height?: number;
    border?:
      | boolean
      | {
          color?: string;
          tolerance?: number;
          keep?: number;
        };
  };
  width?: number;
  height?: number;
  background?: string;
  rotate?: number;
  sharpen?: number;
  blur?: number;
  contrast?: number;
  brightness?: number;
  gamma?: number;
  border?: {
    color?: string;
    width?: number;
    top?: number;
    bottom?: number;
    left?: number;
    right?: number;
  };
  zoom?: number;
};

type ImageDrawOptions = {
  opacity?: number;
  repeat?: boolean | string;
  top?: number;
  left?: number;
  bottom?: number;
  right?: number;
};

type ImageOutputOptions = {
  format:
    | 'image/jpeg'
    | 'image/png'
    | 'image/gif'
    | 'image/webp'
    | 'image/avif'
    | 'rgb'
    | 'rgba';
  quality?: number;
  background?: string;
};

interface ImagesBinding {
  /**
   * Get image metadata (type, width and height)
   * @throws {@link ImagesError} with code 9412 if input is not an image
   * @param stream The image bytes
   */
  info(stream: ReadableStream<Uint8Array>): Promise<ImageInfoResponse>;
  /**
   * Begin applying a series of transformations to an image
   * @param stream The image bytes
   * @returns A transform handle
   */
  input(stream: ReadableStream<Uint8Array>): ImageTransformer;
}

interface ImageTransformer {
  /**
   * Apply transform next, returning a transform handle.
   * You can then apply more transformations, draw, or retrieve the output.
   * @param transform
   */
  transform(transform: ImageTransform): ImageTransformer;

  /**
   * Draw an image on this transformer, returning a transform handle.
   * You can then apply more transformations, draw, or retrieve the output.
   * @param image The image (or transformer that will give the image) to draw
   * @param options The options configuring how to draw the image
   */
  draw(
    image: ReadableStream<Uint8Array> | ImageTransformer,
    options?: ImageDrawOptions
  ): ImageTransformer;

  /**
   * Retrieve the image that results from applying the transforms to the
   * provided input
   * @param options Options that apply to the output e.g. output format
   */
  output(options: ImageOutputOptions): Promise<ImageTransformationResult>;
}

interface ImageTransformationResult {
  /**
   * The image as a response, ready to store in cache or return to users
   */
  response(): Response;
  /**
   * The content type of the returned image
   */
  contentType(): string;
  /**
   * The bytes of the response
   */
  image(): ReadableStream<Uint8Array>;
}

interface ImagesError extends Error {
  readonly code: number;
  readonly message: string;
  readonly stack?: string;
}
