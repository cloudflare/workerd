import { baseConfig } from '../../tools/base.eslint.config.mjs';

export default [
  ...baseConfig(),
  {
    rules: {
      'workerd/no-export-default-of-import-star': 'error',
    },
  },
];
