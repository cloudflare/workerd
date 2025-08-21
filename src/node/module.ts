// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as moduleUtil } from 'node-internal:module';
import {
  ERR_INVALID_ARG_VALUE,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';

export function enableCompileCache(): void {
  // TODO: Investigate the possibility of how to implement this in the future
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.enableCompileCache');
}

export function getCompileCacheDir(): void {
  // We don't plan to support this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.getCompileCacheDir');
}

export const _extensions = {
  '.js': (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('module._extensions.js');
  },
  '.json': (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('module._extensions.json');
  },
  '.node': (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('module._extensions.node');
  },
};

export const createRequire = Object.assign(
  function createRequire(path: string | URL): (specifier: string) => unknown {
    // Note that per Node.js' requirements, path must be one of either
    // an absolute file path or a file URL. We do not currently handle
    // module specifiers as URLs yet, but we'll try to get close.

    const normalizedPath = `${path}`;
    if (
      !normalizedPath.startsWith('/') &&
      !normalizedPath.startsWith('file:')
    ) {
      throw new ERR_INVALID_ARG_VALUE(
        'path',
        normalizedPath,
        'The argument must be a file URL object, a file URL string, or an absolute path string.'
      );
    }

    return moduleUtil.createRequire(normalizedPath);
  },
  {
    resolve: Object.assign(
      () => {
        throw new ERR_METHOD_NOT_IMPLEMENTED('module.createRequire.resolve');
      },
      {
        paths(): void {
          throw new ERR_METHOD_NOT_IMPLEMENTED(
            'module.createRequire.resolve.paths'
          );
        },
      }
    ),
    // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
    cache: Object.create(null),
    extensions: _extensions,
    main: undefined,
  }
);

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

export function register(): void {
  // TODO(soon): We might support this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.register');
}

export function runMain(): void {
  // We don't plan to support this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.runMain');
}

export function syncBuiltinESMExports(): void {
  // TODO(soon): Investigate the possibility of supporting this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.syncBuiltinESMExports');
}

export function wrap(): void {
  // TODO(soon): Investigate the possibility of supporting this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.wrap');
}

export const globalPaths: string[] = [];

export const constants = {
  compileCacheStatus: {
    __proto__: null,
    FAILED: 0,
    ENABLED: 1,
    ALREADY_ENABLED: 2,
    DISABLED: 3,
  },
};

export const _cache = { __proto__: null };
export const _pathCache = { __proto__: null };

export function _debug(): void {
  // This is deprecated and will be removed in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._debug');
}

export function _findPath(): void {
  // It doesn't make sense to support this.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._findPath');
}

export function _initPaths(): void {
  // It doesn't make sense to support this.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._initPaths');
}

export function _load(): void {
  // TODO(soon): Investigate the possibility of supporting this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._load');
}

export function _preloadModules(): void {
  // It doesn't make sense to support this.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._preloadModules');
}

export function _resolveFilename(): void {
  // TODO(soon): Investigate the possibility of supporting this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._resolveFilename');
}

export function _resolveLookupPaths(): void {
  // TODO(soon): Investigate the possibility of supporting this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._resolveLookupPaths');
}

export function _nodeModulePaths(): void {
  // It doesn't make sense to support this.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module._nodeModulePaths');
}

export function findSourceMap(): void {
  // TODO(soon): Investigate the possibility of supporting this in the future.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.findSourceMap');
}

export function findPackageJSON(): void {
  // It doesn't make sense to support this.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.findPackageJSON');
}

export function flushCompileCache(): void {
  // We don't implement compile cache.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.flushCompileCache');
}

export function getSourceMapsSupport(): Record<string, boolean | null> {
  return Object.freeze({
    __proto__: null,
    enabled: false,
    nodeModules: false,
    generatedCode: false,
  });
}

export function setSourceMapsSupport(): void {
  // We don't implement source maps support.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.setSourceMapsSupport');
}

export function stripTypeScriptTypes(): void {
  // We don't implement stripping TypeScript types.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.stripTypeScriptTypes');
}

export function SourceMap(): void {
  // We don't support source maps.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.SourceMap');
}

export function Module(): void {
  // TODO(soon): Investigate implementing Module class fully.
  throw new ERR_METHOD_NOT_IMPLEMENTED('module.Module');
}

Module.register = register;
Module.constants = constants;
Module.enableCompileCache = enableCompileCache;
Module.findPackageJSON = findPackageJSON;
Module.flushCompileCache = flushCompileCache;
Module.getCompileCacheDir = getCompileCacheDir;
Module.stripTypeScriptTypes = stripTypeScriptTypes;

// SourceMap APIs
Module.findSourceMap = findSourceMap;
Module.SourceMap = SourceMap;
Module.getSourceMapsSupport = getSourceMapsSupport;
Module.setSourceMapsSupport = setSourceMapsSupport;

export default {
  builtinModules,
  constants,
  createRequire,
  enableCompileCache,
  findSourceMap,
  getCompileCacheDir,
  getSourceMapsSupport,
  globalPaths,
  isBuiltin,
  register,
  runMain,
  setSourceMapsSupport,
  stripTypeScriptTypes,
  syncBuiltinESMExports,
  wrap,
  flushCompileCache,
  findPackageJSON,
  _cache,
  _debug,
  _extensions,
  _findPath,
  _initPaths,
  _load,
  _pathCache,
  _preloadModules,
  _resolveFilename,
  _resolveLookupPaths,
  _nodeModulePaths,
  Module,
  SourceMap,
};
