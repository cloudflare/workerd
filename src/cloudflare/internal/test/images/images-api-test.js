// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// @ts-ignore
import * as assert from 'node:assert';

function inputStream(body) {
  return new Blob([body]).stream();
}

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
    const info = await env.images.info(inputStream('png'));
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
    const info = await env.images.info(inputStream('<svg></svg>'));
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
    /**
     * @type {any} e;
     */
    let e;

    try {
      await env.images.info(inputStream('BAD'));
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
      transforms: [{ imageIndex: 0, rotate: 90 }],
    });
  },
};

export const test_images_nested_draw = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    const result = await env.images
      .input(inputStream('png'))
      .transform({ rotate: 90 })
      .draw(env.images.input(inputStream('png1')).transform({ rotate: 180 }))
      .draw(
        env.images
          .input(inputStream('png2'))
          .draw(inputStream('png3'))
          .transform({ rotate: 270 })
      )
      .draw(inputStream('png4'))
      .output({ format: 'image/avif' });

    // Would be image/avif in real life, but mock always returns JSON
    assert.equal(result.contentType(), 'application/json');
    const body = await result.response().json();

    assert.deepStrictEqual(body, {
      image: 'png',
      draw_image: ['png1', 'png2', 'png3', 'png4'],
      output_format: 'image/avif',
      transforms: [
        { imageIndex: 0, rotate: 90 },
        { imageIndex: 1, rotate: 180 },
        { drawImageIndex: 1, targetImageIndex: 0 },
        { drawImageIndex: 3, targetImageIndex: 2 },
        { imageIndex: 2, rotate: 270 },
        { drawImageIndex: 2, targetImageIndex: 0 },
        { drawImageIndex: 4, targetImageIndex: 0 },
      ],
    });
  },
};

export const test_images_transformer_draw_twice_disallowed = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    /**
     * @type {any} e;
     */
    let e;

    let t = env.images.input(inputStream('png1'));

    try {
      await env.images
        .input(inputStream('png'))
        .draw(t)
        .draw(t)
        .output({ format: 'image/avif' });
    } catch (e1) {
      e = e1;
    }

    assert.equal(true, !!e);
    assert.equal(e.code, 9525);
    assert.equal(
      e.message,
      'IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() or draw a transformer once'
    );
  },
};

export const test_images_transformer_already_consumed_disallowed = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    /**
     * @type {any} e;
     */
    let e;

    let t = env.images.input(inputStream('png1'));

    await t.output({});

    try {
      await env.images
        .input(inputStream('png'))
        .draw(t)
        .output({ format: 'image/avif' });
    } catch (e1) {
      e = e1;
    }

    assert.equal(true, !!e);
    assert.equal(e.code, 9525);
    assert.equal(
      e.message,
      'IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() or draw a transformer once'
    );
  },
};

export const test_images_transform_bad = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    /**
     * @type {any} e;
     */
    let e;

    try {
      await env.images
        .input(inputStream('BAD'))
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
    /**
     * @type {any} e;
     */
    let e;

    try {
      let transformer = env.images
        .input(inputStream('png'))
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
      'IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() or draw a transformer once'
    );
  },
};
