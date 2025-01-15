import { baseConfig } from '../../../../tools/base.eslint.config.mjs';

export default [
  ...baseConfig({ tsconfigRootDir: import.meta.dirname }),
  {
    files: ['src/workerd/api/wpt/*-test.js'],
    rules: {
      'sort-keys': 'error',
    },
  },
];
