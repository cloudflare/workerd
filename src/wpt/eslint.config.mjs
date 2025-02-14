import { baseConfig } from '../../tools/base.eslint.config.mjs';

export default [
  ...baseConfig({ tsconfigRootDir: import.meta.dirname }),
  {
    files: ['src/wpt/**/*-test.ts'],
    rules: {
      'sort-keys': 'error',
    },
  },
];
