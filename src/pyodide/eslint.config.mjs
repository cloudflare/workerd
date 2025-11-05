import { baseConfig } from '../../tools/base.eslint.config.mjs';

export default [
  ...baseConfig(),
  {
    rules: {
      '@typescript-eslint/no-explicit-any': 'off',
      '@typescript-eslint/no-non-null-assertion': 'off',
      '@typescript-eslint/no-unnecessary-condition': 'off',
      'no-empty': [
        'error',
        {
          allowEmptyCatch: true,
        },
      ],
    },
  },
];
