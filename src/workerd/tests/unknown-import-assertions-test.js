import { rejects } from 'node:assert';

export const test = {
  async test() {
    await rejects(import('worker', { with: { a: 'b' } }), {
      message: /Unrecognized import attributes/,
    });
  },
};
