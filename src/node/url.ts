// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { default as urlUtil } from 'node-internal:url';
import { ERR_MISSING_ARGS } from 'node-internal:internal_errors';
import {
  fileURLToPath,
  pathToFileURL,
  toPathIfFileURL,
  urlToHttpOptions,
} from 'node-internal:internal_url';
import {
  format,
  parse,
  resolve,
  resolveObject,
  Url,
} from 'node-internal:legacy_url';

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

export {
  fileURLToPath,
  pathToFileURL,
  toPathIfFileURL,
  urlToHttpOptions,
} from 'node-internal:internal_url';
export {
  format,
  parse,
  resolve,
  resolveObject,
  Url,
} from 'node-internal:legacy_url';
export { URL, URLSearchParams };

export default {
  domainToASCII,
  domainToUnicode,
  URL,
  URLSearchParams,
  fileURLToPath,
  pathToFileURL,
  toPathIfFileURL,
  urlToHttpOptions,
  // Legacy APIs
  format,
  parse,
  resolve,
  resolveObject,
  Url,
};
