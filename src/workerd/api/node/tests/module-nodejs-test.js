import module from 'node:module';
import { ok } from 'node:assert';

export const testUnimplemented = {
  async test() {
    const fields = [
      '_cache',
      '_pathCache',
      '_extensions',
      'globalPaths',
      '_debug',
      'isBuiltin',
      '_findPath',
      '_nodeModulePaths',
      '_resolveLookupPaths',
      '_load',
      '_resolveFilename',
      'createRequire',
      '_initPaths',
      '_preloadModules',
      'syncBuiltinESMExports',
      'Module',
      'registerHooks',
      'builtinModules',
      'runMain',
      'register',
      'constants',
      'enableCompileCache',
      'findPackageJSON',
      'flushCompileCache',
      'stripTypeScriptTypes',
      'getCompileCacheDir',
      'findSourceMap',
      'SourceMap',
      'getSourceMapsSupport',
      'setSourceMapsSupport',
    ];

    for (const field of fields) {
      ok(field in module, `${field} is not in module`);
      ok(module[field] != null, `${field} is ${typeof module[field]}`);
    }
  },
};
