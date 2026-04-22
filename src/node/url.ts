// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { default as urlUtil } from 'node-internal:url';
import { ERR_MISSING_ARGS } from 'node-internal:internal_errors';
import { Buffer } from 'node-internal:internal_buffer';
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
// URLPattern is a global in workerd; re-exported here for Node.js compat parity.
// URLPattern is not declared on lib.dom's globalThis, so cast to unknown first.
export const URLPattern = (globalThis as unknown as { URLPattern: unknown })
  .URLPattern;

// Node.js-only helper that returns the decoded file URL path as a Buffer.
export function fileURLToPathBuffer(
  url: string | URL,
  options?: { windows?: boolean }
): Buffer {
  return Buffer.from(fileURLToPath(url, options));
}

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
  URLPattern,
  URLSearchParams,
  fileURLToPath,
  fileURLToPathBuffer,
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
