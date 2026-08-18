// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

export const handler: ExportedHandler<{ IMAGES: ImagesBinding }> = {
  async fetch(request, env) {
    const result = await env.IMAGES.input(request.body!).output({
      format: 'image/webp',
    });

    // response() can be called with no arguments, as before.
    expectType<Response>(result.response());

    // response() accepts additional headers, e.g. for cache control.
    expectType<Response>(
      result.response({
        headers: {
          'Cache-Control': 'public, max-age=3600, stale-while-revalidate=86400',
        },
      })
    );

    // headers can also be a Headers instance or an iterable of pairs.
    expectType<Response>(
      result.response({ headers: new Headers({ 'Cache-Tag': 'my-tag' }) })
    );
    expectType<Response>(
      result.response({ headers: [['Cache-Tag', 'my-tag']] })
    );

    return result.response();
  },
};
