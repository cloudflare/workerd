// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { urlToHttpOptions, isURL } from 'node-internal:internal_url';
import { ClientRequest } from 'node-internal:internal_http_client';
import { Agent, globalAgent } from 'node-internal:internal_https_agent';
import type { IncomingMessageCallback } from 'node-internal:internal_http_util';
import type { RequestOptions } from 'node:http';

export function request(
  url: string | URL | RequestOptions,
  options?: RequestOptions | IncomingMessageCallback,
  cb?: IncomingMessageCallback
): ClientRequest;
export function request(...args: unknown[]): ClientRequest {
  let options: RequestOptions = {};

  if (typeof args[0] === 'string') {
    const urlStr = args.shift() as string;
    options = urlToHttpOptions(new URL(urlStr));
  } else if (isURL(args[0])) {
    options = urlToHttpOptions(args.shift() as URL);
  }

  if (args.length > 0 && typeof args[0] !== 'function') {
    Object.assign(options, args.shift());
  }

  options._defaultAgent = globalAgent;
  args.unshift(options);

  // @ts-expect-error TS2556 This is OK.
  return new ClientRequest(...args);
}

export function get(
  input: string | URL | RequestOptions,
  options?: RequestOptions | IncomingMessageCallback,
  cb?: IncomingMessageCallback
): ClientRequest {
  const req = request(input, options, cb);
  req.end();
  return req;
}

export { Agent, globalAgent };

export default {
  get,
  request,
  Agent,
  globalAgent,
};
