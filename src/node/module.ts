// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as moduleUtil } from 'node-internal:module';
import {
  ERR_INVALID_ARG_VALUE,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';

export function createRequire(
  path: string | URL
): (specifier: string) => unknown {
  // Note that per Node.js' requirements, path must be one of either
  // an absolute file path or a file URL. We do not currently handle
  // module specifiers as URLs yet, but we'll try to get close.

  const normalizedPath = `${path}`;
  if (!normalizedPath.startsWith('/') && !normalizedPath.startsWith('file:')) {
    throw new ERR_INVALID_ARG_VALUE(
      'path',
      normalizedPath,
      'The argument must be a file URL object, a file URL string, or an absolute path string.'
    );
  }

  // TODO(soon): We should move this to C++ land.
  // Ref: https://nodejs.org/docs/latest/api/modules.html#requireid
  return Object.assign(moduleUtil.createRequire(normalizedPath), {
    // We don't throw ERR_METHOD_NOT_IMPLEMENTED because it's too disruptive.
    resolve(): void {
      return undefined;
    },
    // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
    cache: Object.create(null),
    main: undefined,
  });
}

// Indicates only that the given specifier is known to be a
// Node.js built-in module specifier with or with the the
// 'node:' prefix. A true return value does not guarantee that
// the module is actually implemented in the runtime.
export const isBuiltin = moduleUtil.isBuiltin.bind(moduleUtil);

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

export function findSourceMap(): void {
  // Not meaningful to implement in the context of workerd.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.findSourceMap');
}

export function register(): void {
  // Not meaningful to implement in the context of workerd.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.register');
}

export function syncBuiltinESMExports(): void {
  // Not meaningful to implement in the context of workerd.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.syncBuiltinESMExports');
}

//
// IMPORTANT NOTE!
//
// We are deliberately not including any of these functions below because
// they are either experimental, in active development or not relevant
// as of January 2025.
//
// - module.registerHooks()
// - module.stripTypeScriptTypes()
// - module.SourceMap
// - module.constants.compileCacheStatus
// - module.enableCompileCache()
// - module.flushCompileCache()
// - module.getCompileCacheDir()
//

export default {
  createRequire,
  isBuiltin,
  builtinModules,
  findSourceMap,
  register,
  syncBuiltinESMExports,
};
