import { default as Debug } from 'workerd:debug';
import { strictEqual } from 'node:assert';

export const autogateIsEnabledTest = {
  // Test to ensure that async context is propagated into custom thenables.
  async test() {
    strictEqual(Debug.autogateIsEnabled('workerd-autogate-test-workerd'), true);
  },
};
