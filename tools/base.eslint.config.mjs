import eslint from "@eslint/js";
import tseslint from "typescript-eslint";

/**
 * @param {string} tsconfigRootDir
 * @returns {FlatConfig.ConfigArray}
 */
export function baseConfig({ tsconfigRootDir }) {
  return tseslint.config(
    eslint.configs.recommended,
    ...tseslint.configs.strictTypeChecked,
    {
      languageOptions: {
        parserOptions: {
          ecmaVersion: "latest",
          sourceType: "module",
          projectService: true,
          tsconfigRootDir,
        },
      },
      rules: {
        "@typescript-eslint/explicit-function-return-type": "error",
        "@typescript-eslint/explicit-member-accessibility": "error",
        "@typescript-eslint/explicit-module-boundary-types": "error",
        "@typescript-eslint/no-require-imports": "error",
        "@typescript-eslint/prefer-enum-initializers": "error",
        "@typescript-eslint/restrict-template-expressions": "warn",
        "@typescript-eslint/no-non-null-assertion": "warn",
        "@typescript-eslint/no-extraneous-class": "off",
        "@typescript-eslint/unified-signatures": "off",
        "@typescript-eslint/no-unused-vars": [
          "error",
          {
            args: "all",
            argsIgnorePattern: "^_",
            caughtErrors: "all",
            caughtErrorsIgnorePattern: "^_",
            destructuredArrayIgnorePattern: "^_",
            varsIgnorePattern: "^_",
            ignoreRestSiblings: true,
          },
        ],
      },
    },
    {
      files: ['**/*.js', '**/*.mjs', '**/*.cjs'],
      ...tseslint.configs.disableTypeChecked,
    },
  )
}
