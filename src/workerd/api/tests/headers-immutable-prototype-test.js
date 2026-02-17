import { strictEqual } from 'node:assert';

export const test = {
  test() {
    strictEqual(
      Reflect.getOwnPropertyDescriptor(Headers, 'prototype').writable,
      false
    );
  },
};
