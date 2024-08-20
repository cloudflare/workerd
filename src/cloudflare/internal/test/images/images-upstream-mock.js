// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * @param {FormData} form
 * @returns {Promise<string | null>}
 */
async function imageAsString(form) {
  let blob = form.get('image');

  if (blob === null) {
    return null;
  }

  if (typeof blob === 'string') {
    return null;
  }

  return blob.text();
}

export default {
  /**
   * @param {Request} request
   */
  async fetch(request) {
    const form = await request.formData();
    const image = (await imageAsString(form)) || '';
    if (image.includes('BAD')) {
      const resp = new Response('ERROR 123: Bad request', {
        status: 409,
        headers: {
          'cf-images-binding': 'err=123',
        },
      });
      return resp;
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
      case '/transform':
        /**
         * @type {any}
         */
        const obj = {
          image: await imageAsString(form),
          // @ts-ignore
          transforms: JSON.parse(form.get('transforms') || '{}'),
        };
        for (const x of ['output_format', 'output_quality', 'background']) {
          if (form.get(x)) {
            obj[x] = form.get(x);
          }
        }

        return Response.json(obj);
    }

    throw new Error('Unexpected mock invocation');
  },
};
