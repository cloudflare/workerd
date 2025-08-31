import eslint from '@eslint/js';
import tseslint from 'typescript-eslint';

/**
 * @returns {import('typescript-eslint').ConfigArray}
 */
export function baseConfig() {
  return tseslint.config(
    eslint.configs.recommended,
    ...tseslint.configs.strictTypeChecked,
    {
      languageOptions: {
        parserOptions: {
          ecmaVersion: 'latest',
          sourceType: 'module',
          projectService: true,
          tsconfigRootDir: import.meta.dirname,
          jsDocParsingMode: 'all',
        },
      },
      rules: {
        '@typescript-eslint/explicit-function-return-type': 'error',
        '@typescript-eslint/explicit-member-accessibility': [
          'error',
          { accessibility: 'no-public' },
        ],
        '@typescript-eslint/explicit-module-boundary-types': 'off',
        '@typescript-eslint/no-require-imports': 'error',
        '@typescript-eslint/prefer-enum-initializers': 'error',
        '@typescript-eslint/restrict-template-expressions': 'off',
        '@typescript-eslint/no-non-null-assertion': 'error',
        '@typescript-eslint/no-extraneous-class': 'off',
        '@typescript-eslint/unified-signatures': 'off',
        '@typescript-eslint/no-unused-vars': [
          'error',
          {
            args: 'all',
            argsIgnorePattern: '^_',
            caughtErrors: 'all',
            caughtErrorsIgnorePattern: '^_',
            destructuredArrayIgnorePattern: '^_',
            varsIgnorePattern: '^_',
            ignoreRestSiblings: true,
          },
        ],
        'no-restricted-syntax': [
          'error',
          {
            selector: "MethodDefinition[accessibility='private']",
            message:
              "Use private field syntax (#) instead of 'private' keyword for methods",
          },
          {
            selector:
              "PropertyDefinition[accessibility='private']:not([computed=true])",
            message:
              "Use private field syntax (#) instead of 'private' keyword for simple properties",
          },
          {
            selector: "TSParameterProperty[accessibility='private']",
            message:
              "Use private field syntax (#) instead of 'private' keyword for constructor parameters",
          },
        ],
      },
    },
    {
      files: ['**/*.js', '**/*.mjs', '**/*.cjs'],
      ...tseslint.configs.disableTypeChecked,
    },
    {
      files: ['**/*.js', '**/*.mjs', '**/*.cjs'],
      rules: {
        '@typescript-eslint/explicit-function-return-type': 'off',
      },
    },
  )
}
