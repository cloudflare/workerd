import { baseConfig } from '../../tools/base.eslint.config.mjs';
import customRules from '../../tools/custom-eslint-rules.mjs';

export default [
  ...baseConfig(),
  {
    plugins: {
      workerd: customRules,
    },
    rules: {
      'workerd/no-export-default-of-import-star': 'error',
    },
  },
];
