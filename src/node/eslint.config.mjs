import { baseConfig } from '../../tools/base.eslint.config.mjs';

export default [...baseConfig({ tsconfigRootDir: import.meta.dirname })];
