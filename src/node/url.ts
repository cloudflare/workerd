// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
/* eslint-disable @typescript-eslint/no-unsafe-assignment */
import { default as urlUtil } from 'node-internal:url';
import { ERR_MISSING_ARGS } from 'node-internal:internal_errors';

const { URL, URLSearchParams } = globalThis;

export function domainToASCII(domain?: unknown): string {
  if (arguments.length < 1) {
    throw new ERR_MISSING_ARGS('domain');
  }

  return urlUtil.domainToASCII(`${domain}`);
}

export function domainToUnicode(domain?: unknown): string {
  if (arguments.length < 1) {
    throw new ERR_MISSING_ARGS('domain');
  }

  return urlUtil.domainToUnicode(`${domain}`);
}

export { URL, URLSearchParams };

export default {
  domainToASCII,
  domainToUnicode,
  URL,
  URLSearchParams,
};
