// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A mix of the APIs that real Workers use, so that the profile doesn't fit only hello world. Each
// path exercises a different part of the runtime.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    switch (url.pathname) {
      case '/json': {
        const body = await request.json();
        body.items = body.items.map((item) => ({
          ...item,
          total: item.price * item.qty,
        }));
        return Response.json(body, {
          headers: { 'cache-control': 'no-store' },
        });
      }
      case '/headers': {
        const headers = new Headers();
        for (const [name, value] of request.headers) {
          headers.set(`x-echo-${name}`, value.toUpperCase());
        }
        headers.append('set-cookie', 'a=1; Path=/');
        headers.append('set-cookie', 'b=2; Path=/');
        return new Response(request.headers.get('user-agent') ?? 'none', {
          headers,
        });
      }
      case '/stream': {
        const encoder = new TextEncoder();
        let i = 0;
        const stream = new ReadableStream({
          pull(controller) {
            if (i++ < 8) controller.enqueue(encoder.encode(`chunk ${i}\n`));
            else controller.close();
          },
        });
        return new Response(stream, {
          headers: { 'content-type': 'text/plain' },
        });
      }
      case '/transform': {
        const upper = new TransformStream({
          transform(chunk, controller) {
            const text = new TextDecoder().decode(chunk);
            controller.enqueue(new TextEncoder().encode(text.toUpperCase()));
          },
        });
        const upstream = await env.upstream.fetch('http://upstream/text');
        return new Response(upstream.body.pipeThrough(upper));
      }
      case '/subrequest': {
        const n = url.searchParams.get('n');
        const upstream = await env.upstream.fetch(
          `http://upstream/json?n=${n}`
        );
        const data = await upstream.json();
        return new Response(`got ${data.n} from upstream`);
      }
      case '/crypto': {
        const data = new TextEncoder().encode(url.search.repeat(16));
        const digest = await crypto.subtle.digest('SHA-256', data);
        const hex = [...new Uint8Array(digest)]
          .map((b) => b.toString(16).padStart(2, '0'))
          .join('');
        return new Response(`${hex} ${crypto.randomUUID()}`);
      }
      case '/url': {
        const target = new URL('/a/b/../c?x=1&y=2#frag', 'https://example.com');
        const params = new URLSearchParams(url.search);
        params.append('z', '3');
        return new Response(
          `${target.href} ${params.toString()} ${encodeURIComponent('a b&c')}`
        );
      }
      case '/text': {
        const text = await request.text();
        return new Response(text.split('').reverse().join(''), { status: 201 });
      }
      default:
        return new Response('not found', { status: 404 });
    }
  },
};
