import type {
  setImmediate as setImmediateImpl,
  clearImmediate as clearImmediateImpl,
} from 'node:timers';

export const setImmediate: typeof setImmediateImpl;
export const clearImmediate: typeof clearImmediateImpl;
