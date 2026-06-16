// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// index.mjs
import { Buffer } from 'node:buffer';

const encoder = new TextEncoder();

async function pbkdf2Derive(password) {
  const passwordArray = encoder.encode(password);
  const passwordKey = await crypto.subtle.importKey(
    'raw',
    passwordArray,
    'PBKDF2',
    false,
    ['deriveBits']
  );
  const saltArray = crypto.getRandomValues(new Uint8Array(16));
  const keyBuffer = await crypto.subtle.deriveBits(
    { name: 'PBKDF2', hash: 'SHA-256', salt: saltArray, iterations: 1_000_000 },
    passwordKey,
    256
  );
  return Buffer.from(keyBuffer).toString('base64');
}

export default {
  async fetch(request, env, ctx) {
    if (request.url.includes('/pbkdf2Derive')) {
      return new Response(await pbkdf2Derive('hello!'));
    }
    if (request.url.includes('/throwException')) {
      const url = new URL(request.url);
      throw new Error(url.searchParams.get('message'));
    }
    return new Response('Not found', { status: 404 });
  },
};
