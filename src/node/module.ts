// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
/* eslint-disable */

import { default as moduleUtil } from 'node-internal:module';
import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';

export function createRequire(
  path: string | URL
): (specifier: string) => unknown {
  // Note that per Node.js' requirements, path must be one of either
  // an absolute file path or a file URL. We do not currently handle
  // module specifiers as URLs yet but we'll try to get close.

  path = `${path}`;
  if (
    !(path as string).startsWith('/') &&
    !(path as string).startsWith('file:')
  ) {
    throw new ERR_INVALID_ARG_VALUE(
      'path',
      path,
      'The argument must be a file URL object, ' +
        'a file URL string, or an absolute path string.'
    );
  }

  return moduleUtil.createRequire(path as string);
}

// Indicates only that the given specifier is known to be a
// Node.js built-in module specifier with or with the the
// 'node:' prefix. A true return value does not guarantee that
// the module is actually implemented in the runtime.
export function isBuiltin(specifier: string): boolean {
  return moduleUtil.isBuiltin(specifier);
}

// Intentionally does not include modules with mandatory 'node:'
// prefix like `node:test`.
// See: See https://nodejs.org/docs/latest/api/modules.html#built-in-modules-with-mandatory-node-prefix
// TODO(later): This list duplicates the list that is in
// workerd/jsg/modules.c++. Later we should source these
// from the same place so we don't have to maintain two lists.
export const builtinModules = [
  '_http_agent',
  '_http_client',
  '_http_common',
  '_http_incoming',
  '_http_outgoing',
  '_http_server',
  '_stream_duplex',
  '_stream_passthrough',
  '_stream_readable',
  '_stream_transform',
  '_stream_wrap',
  '_stream_writable',
  '_tls_common',
  '_tls_wrap',
  'assert',
  'assert/strict',
  'async_hooks',
  'buffer',
  'child_process',
  'cluster',
  'console',
  'constants',
  'crypto',
  'dgram',
  'diagnostics_channel',
  'dns',
  'dns/promises',
  'domain',
  'events',
  'fs',
  'fs/promises',
  'http',
  'http2',
  'https',
  'inspector',
  'inspector/promises',
  'module',
  'net',
  'os',
  'path',
  'path/posix',
  'path/win32',
  'perf_hooks',
  'process',
  'punycode',
  'querystring',
  'readline',
  'readline/promises',
  'repl',
  'stream',
  'stream/consumers',
  'stream/promises',
  'stream/web',
  'string_decoder',
  'sys',
  'timers',
  'timers/promises',
  'tls',
  'trace_events',
  'tty',
  'url',
  'util',
  'util/types',
  'v8',
  'vm',
  'wasi',
  'worker_threads',
  'zlib',
];
Object.freeze(builtinModules);

export default {
  createRequire,
  isBuiltin,
  builtinModules,
};
