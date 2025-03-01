import { baseConfig } from '../../tools/base.eslint.config.mjs';

export default [
  ...baseConfig(),
  {
    files: ['src/wpt/**/*-test.ts'],
    rules: {
      'sort-keys': 'error',
    },
  },
];
