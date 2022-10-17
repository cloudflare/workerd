module.exports = {
  parser: "@typescript-eslint/parser",
  extends: ["plugin:prettier/recommended"],
  plugins: ["import"],
  rules: {
    "import/order": ["warn", { alphabetize: { order: "asc" } }],
    "sort-imports": ["warn", { ignoreDeclarationSort: true }],
  },
  overrides: [
    {
      files: ["*.ts"],
      extends: ["plugin:@typescript-eslint/recommended"],
      rules: {
        "@typescript-eslint/ban-ts-comment": "off",
        "@typescript-eslint/no-non-null-assertion": "off",
        "@typescript-eslint/no-explicit-any": "off",
        "@typescript-eslint/no-empty-function": "off",
        "@typescript-eslint/no-unused-vars": [
          "warn",
          { argsIgnorePattern: "^_" },
        ],
      },
    },
  ],
};
