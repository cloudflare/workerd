// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

/**
 * @param {FormDataEntryValue | null} blob
 * @returns {Promise<string | null>}
 */
async function imageAsString(blob) {
  if (blob === null) {
    return null;
  }

  if (typeof blob === 'string') {
    return null;
  }

  return blob.text();
}

export class ServiceEntrypoint extends WorkerEntrypoint {
  /**
   * @param {string} imageId
   * @returns {Promise<ImageMetadata | null>}
   */
  async get(imageId) {
    if (imageId === 'not-found') {
      return null;
    }

    return {
      id: imageId,
      filename: 'test.jpg',
      uploaded: '2024-01-01T00:00:00Z',
      requireSignedURLs: false,
      variants: ['public'],
      meta: {},
      draft: false,
    };
  }

  async getImage(imageId) {
    if (imageId === 'not-found') {
      return null;
    }

    const mockData = `MOCK_IMAGE_DATA_${imageId}`;
    return new Blob([mockData]).stream();
  }

  async upload(image, options) {
    // Handle both ReadableStream and ArrayBuffer
    const buffer =
      image instanceof ArrayBuffer
        ? image
        : await new Response(image).arrayBuffer();

    const decoder = new TextDecoder();
    const text = decoder.decode(buffer);

    if (text === 'INVALID') {
      throw new Error('Invalid image data');
    }

    return {
      id: options?.id || 'generated-id',
      filename: options?.filename || 'uploaded.jpg',
      uploaded: '2024-01-01T00:00:00Z',
      requireSignedURLs: options?.requireSignedURLs || false,
      variants: ['public'],
      meta: options?.metadata || {},
      draft: false,
    };
  }

  /**
   * @param {string} imageId
   * @param {ImageUpdateOptions} body
   * @returns {Promise<ImageMetadata>}
   */
  async update(imageId, body) {
    if (imageId === 'not-found') {
      throw new Error('Image not found');
    }

    return {
      id: imageId,
      filename: 'updated.jpg',
      uploaded: '2024-01-01T00:00:00Z',
      requireSignedURLs:
        body.requireSignedURLs !== undefined ? body.requireSignedURLs : false,
      variants: ['public'],
      meta: body.metadata || {},
      draft: false,
    };
  }

  /**
   * @param {string} imageId
   * @returns {Promise<boolean>}
   */
  async delete(imageId) {
    return imageId !== 'not-found';
  }

  /**
   * @param {ImageListOptions} [options]
   * @returns {Promise<ImageList>}
   */
  async list(options) {
    const images = [
      {
        id: 'image-1',
        filename: 'test1.jpg',
        uploaded: '2024-01-01T00:00:00Z',
        requireSignedURLs: false,
        variants: ['public'],
        meta: {},
      },
      {
        id: 'image-2',
        filename: 'test2.jpg',
        uploaded: '2024-01-02T00:00:00Z',
        requireSignedURLs: false,
        variants: ['public'],
        meta: {},
      },
    ];

    const limit = options?.limit || 50;
    const slicedImages = images.slice(0, limit);

    return {
      images: slicedImages,
      listComplete: true,
    };
  }

  /**
   * Handle HTTP requests for info and transform operations.
   * In production these go to a separate transformation service,
   * but in tests we mock both the ServiceEntrypoint and transformation service in one place.
   * @param {Request} request
   * @returns {Promise<Response>}
   */
  async fetch(request) {
    const form = await request.formData();
    const image = (await imageAsString(form.get('image'))) || '';
    if (image.includes('BAD')) {
      return new Response('ERROR 123: Bad request', {
        status: 409,
        headers: {
          'cf-images-binding': 'err=123',
        },
      });
    }

    switch (new URL(request.url).pathname) {
      case '/info':
        if (image.includes('<svg')) {
          return Response.json({
            format: 'image/svg+xml',
          });
        } else {
          return Response.json({
            format: 'image/png',
            file_size: 123,
            width: 123,
            height: 123,
          });
        }
      case '/transform': {
        /** @type {any} */
        const obj = {
          image: await imageAsString(form.get('image')),
          // @ts-ignore
          transforms: JSON.parse(form.get('transforms') || '{}'),
        };
        for (const x of [
          'output_format',
          'output_quality',
          'background',
          'anim',
        ]) {
          if (form.get(x)) {
            obj[x] = form.get(x);
          }
        }

        if (form.get('draw_image')) {
          const drawImages = [];
          for (const entry of form.getAll('draw_image')) {
            drawImages.push(await imageAsString(entry));
          }
          obj['draw_image'] = drawImages;
        }

        return Response.json(obj);
      }
    }

    throw new Error('Unexpected mock invocation');
  }
}

export default {
  /**
   * @param {Request} request
   * @param {*} env
   * @param {ExecutionContext} ctx
   * @returns {Promise<Response>}
   */
  async fetch(request, env, ctx) {
    const entrypoint = new ServiceEntrypoint(ctx, env);
    return entrypoint.fetch(request);
  },
};
