import { rejects } from 'node:assert';

export const test = {
  async test() {
    await rejects(import('node:crypto'), {
      message: /^No such module/,
    });
    await rejects(import('node:buffer'), {
      message: /^No such module/,
    });
  },
};
