// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// @ts-ignore
import * as assert from 'node:assert';

const encoder = new TextEncoder();
function inputStream(chunks) {
  return new ReadableStream({
    start(controller) {
      for (const chunk of chunks) {
        controller.enqueue(encoder.encode(chunk));
      }
      controller.close();
    },
  });
}

/**
 * @typedef {{'images': ImagesBinding}} Env
 *
 */

/**
 * @param {Env} env
 * @param {string[]} chunks
 * @returns {Promise<string>}
 */
async function decodeBase64ThroughImagesBinding(env, chunks) {
  const result = await env.images
    .input(inputStream(chunks), { encoding: 'base64' })
    .output({ format: 'image/avif' });

  const body = await result.response().json();
  return body.image;
}

export const test_images_info_bitmap = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    const info = await env.images.info(inputStream(['png']));
    assert.deepStrictEqual(info, {
      format: 'image/png',
      fileSize: 123,
      width: 123,
      height: 123,
    });
  },
};

export const test_images_info_bitmap_base64 = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    const info = await env.images.info(inputStream([btoa('png')]), {
      encoding: 'base64',
    });
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
    const info = await env.images.info(inputStream(['<svg></svg>']));
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
      await env.images.info(inputStream(['BAD']));
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
    const result = await env.images
      .input(inputStream(['png']))
      .transform({ rotate: 90 })
      .output({ format: 'image/avif' });

    // Would be image/avif in real life, but mock always returns JSON
    assert.strictEqual(result.contentType(), 'application/json');
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
      .input(inputStream(['png']))
      .transform({ rotate: 90 })
      .draw(env.images.input(inputStream(['png1'])).transform({ rotate: 180 }))
      .draw(
        env.images
          .input(inputStream(['png2']))
          .draw(inputStream(['png3']))
          .transform({ rotate: 270 })
      )
      .draw(inputStream(['png4']))
      .output({ format: 'image/avif' });

    // Would be image/avif in real life, but mock always returns JSON
    assert.strictEqual(result.contentType(), 'application/json');
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

    let t = env.images.input(inputStream(['png1']));

    try {
      await env.images
        .input(inputStream(['png']))
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

    let t = env.images.input(inputStream(['png1']));

    await t.output({});

    try {
      await env.images
        .input(inputStream(['png']))
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
        .input(inputStream(['BAD']))
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
        .input(inputStream(['png']))
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

const ENCODE_BLOCKSIZE = 32 * 1024 + 1;
const DECODE_BLOCKSIZE = 32 * 1024;

export const test_images_base64_input = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    const inputs = {
      smallInput: 'QUFB',
      blockSizeInput: 'QUFB'.repeat(DECODE_BLOCKSIZE / 'QUFB'.length),
      twoBlockInput: 'QUFB'.repeat(DECODE_BLOCKSIZE / 'QUFB'.length + 1),
      hugeInput: 'QUFB'.repeat((DECODE_BLOCKSIZE * 20) / 'QUFB'.length),
    };

    for (let testName in inputs) {
      const input = inputs[testName];

      const result = await env.images
        .input(inputStream([input]), { encoding: 'base64' })
        .transform({ rotate: 90 })
        .output({ format: 'image/avif' });

      // Would be image/avif in real life, but mock always returns JSON
      assert.strictEqual(result.contentType(), 'application/json');
      const body = await result.response().json();

      assert.deepStrictEqual(body, {
        image: atob(input),
        output_format: 'image/avif',
        transforms: [{ imageIndex: 0, rotate: 90 }],
      });
    }
  },
};

export const test_images_base64_output = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */

  async test(_, env) {
    // We can't really control the length of the output here as it also has
    // a JSON body around it
    const inputs = {
      smallInput: 'A',
      twoBlockInput: 'A'.repeat(ENCODE_BLOCKSIZE + 1),
      hugeInput: 'A'.repeat(ENCODE_BLOCKSIZE * 20),
    };

    for (const testName in inputs) {
      const input = inputs[testName];

      const result = await env.images
        .input(inputStream([input]))
        .transform({ rotate: 90 })
        .output({ format: 'image/avif' });

      // Would be image/avif in real life, but mock always returns JSON
      assert.strictEqual(result.contentType(), 'application/json');
      const bodyBase64 = await new Response(
        await result.image({ encoding: 'base64' })
      ).text();
      const body = JSON.parse(atob(bodyBase64));

      assert.deepStrictEqual(body, {
        image: input,
        output_format: 'image/avif',
        transforms: [{ imageIndex: 0, rotate: 90 }],
      });
    }
  },
};

export const test_images_base64_empty_input = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    assert.strictEqual(await decodeBase64ThroughImagesBinding(env, []), '');
  },
};

export const test_images_base64_padding = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, ['QQ==']),
      'A'
    );
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, ['QWI=']),
      'Ab'
    );
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, ['QWJj']),
      'Abc'
    );
  },
};

export const test_images_base64_chunk_boundaries = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, ['QUFB', 'QUFB']),
      'AAAAAA'
    );
  },
};

export const test_images_base64_small_chunks = {
  /**
   * @param {unknown} _
   * @param {Env} env
   */
  async test(_, env) {
    // 1 byte chunks
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, [
        'Q',
        'U',
        'F',
        'B',
        'Q',
        'U',
        'F',
        'B',
      ]),
      'AAAAAA'
    );

    // 2 byte chunks
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, ['QU', 'FB', 'QU', 'FB']),
      'AAAAAA'
    );

    // 3 byte chunks
    assert.strictEqual(
      await decodeBase64ThroughImagesBinding(env, ['QUF', 'BQU', 'FB']),
      'AAAAAA'
    );
  },
};
