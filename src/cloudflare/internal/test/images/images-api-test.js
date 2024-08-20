// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// @ts-ignore
import * as assert from 'node:assert';

/**
 * @typedef {{'images': ImagesBinding}} Env
 *
 */

export const test_images_info_bitmap = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    const blob = new Blob(['png']);
    const info = await env.images.info(blob.stream());
    assert.deepStrictEqual(info, {
      format: 'image/png',
      fileSize: 123,
      width: 123,
      height: 123,
    });
  },
};

export const test_images_info_svg = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    const blob = new Blob(['<svg></svg>']);
    const info = await env.images.info(blob.stream());
    assert.deepStrictEqual(info, {
      format: 'image/svg+xml',
    });
  },
};

export const test_images_info_error = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    const blob = new Blob(['BAD']);

    /**
     * @type {any} e;
     */
    let e;

    try {
      await env.images.info(blob.stream());
    } catch (e2) {
      e = e2;
    }

    assert.equal(true, !!e);
    assert.equal(e.code, 123);
    assert.equal(e.message, 'IMAGES_INFO_ERROR 123: Bad request');
  },
};

export const test_images_transform = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    const blob = new Blob(['png']);

    const result = await env.images
      .input(blob.stream())
      .transform({ rotate: 90 })
      .output({ format: 'image/avif' });

    // Would be image/avif in real life, but mock always returns JSON
    assert.equal(result.contentType(), 'application/json');
    const body = await result.response().json();

    assert.deepStrictEqual(body, {
      image: 'png',
      output_format: 'image/avif',
      transforms: [{ rotate: 90 }],
    });
  },
};

export const test_images_transform_bad = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    const blob = new Blob(['BAD']);

    /**
     * @type {any} e;
     */
    let e;

    try {
      await env.images
        .input(blob.stream())
        .transform({ rotate: 90 })
        .output({ format: 'image/avif' });
    } catch (e2) {
      e = e2;
    }

    assert.equal(true, !!e);
    assert.equal(e.code, 123);
    assert.equal(e.message, 'IMAGES_TRANSFORM_ERROR 123: Bad request');
  },
};

export const test_images_transform_consumed = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    const blob = new Blob(['png']);

    /**
     * @type {any} e;
     */
    let e;

    try {
      let transformer = env.images
        .input(blob.stream())
        .transform({ rotate: 90 });

      await transformer.output({ format: 'image/avif' });
      await transformer.output({ format: 'image/avif' });
    } catch (e2) {
      e = e2;
    }

    assert.equal(true, !!e);
    assert.equal(e.code, 9525);
    assert.equal(
      e.message,
      'IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() once'
    );
  },
};
