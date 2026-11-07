// The bootstrap's scripts are CommonJS, unlike the code that the base config
// in tools/ checks, so they have a config of their own.
import { defineConfig } from 'eslint/config';
import tseslint from 'typescript-eslint';

const commonJsOnly =
  "The bootstrap's scripts export values only with `module.exports = { ... }`; " +
  'use `export type` for types.';

export default defineConfig([
  {
    files: ['**/*.ts'],
    languageOptions: {
      parser: tseslint.parser,
      parserOptions: {
        ecmaVersion: 'latest',
        sourceType: 'module',
      },
    },
    rules: {
      // TypeScript compiles a value export to a getter on `module.exports`,
      // and the deserializer can't call a getter to find a cloneable class:
      // V8 runs no JavaScript while it reads a value (see serialization.ts).
      'no-restricted-syntax': [
        'error',
        {
          selector: "ExportNamedDeclaration[exportKind='value']",
          message: commonJsOnly,
        },
        {
          selector: "ExportAllDeclaration[exportKind='value']",
          message: commonJsOnly,
        },
        { selector: 'ExportDefaultDeclaration', message: commonJsOnly },
        { selector: 'TSExportAssignment', message: commonJsOnly },
      ],
    },
  },
]);
