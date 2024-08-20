import { baseConfig } from '../../tools/base.eslint.config.mjs';

// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
export default [...baseConfig({ tsconfigRootDir: import.meta.dirname })];
