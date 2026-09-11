// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared machinery: requests to the sidecar server (see http-servers.js)
// and small event helpers.

import http from 'node:http';
import { Buffer } from 'node:buffer';

// http.request() against the sidecar's `path`, with any further options.
export function request(env, path, options = {}, callback) {
  return http.request(
    {
      hostname: env.SIDECAR_HOSTNAME,
      port: Number(env.HTTP_SERVER_PORT),
      path,
      ...options,
    },
    callback
  );
}

// request() ended at once: a request without a body.
export function get(env, path, options = {}, callback) {
  return request(env, path, options, callback).end();
}

export function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// The 'response' of a request, or its 'error'.
export function response(req) {
  return new Promise((resolve, reject) => {
    req.once('response', resolve);
    req.once('error', reject);
  });
}

// The body of an IncomingMessage as one Buffer (or string, after
// setEncoding()), or its 'error'.
export function collect(res) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    res.on('data', (chunk) => chunks.push(chunk));
    res.once('end', () =>
      resolve(
        typeof chunks[0] === 'string' ? chunks.join('') : Buffer.concat(chunks)
      )
    );
    res.once('error', reject);
  });
}

// Records the named events of an emitter, as `prefix:event`, with an
// Error argument rendered as `(name/code/message)`.
export function record(log, prefix, emitter, events) {
  for (const event of events) {
    emitter.on(event, (arg) => {
      log.push(
        arg instanceof Error
          ? `${prefix}:${event}(${arg.name}/${arg.code ?? '-'}/${arg.message})`
          : `${prefix}:${event}`
      );
    });
  }
}

// The sidecar's record of a /never-ends?id=ID request: { opened, closed }.
export async function neverEndsStats(env, id) {
  const res = await response(get(env, `/stats?id=${id}`));
  return JSON.parse((await collect(res)).toString());
}

let nextId = 0;
export function uniqueId(prefix) {
  return `${prefix}-${nextId++}`;
}
