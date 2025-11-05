import Console from 'node:console';
import { strictEqual, doesNotThrow, throws } from 'node:assert';

// These methods are retrieved using Object.keys(require('node:console'))
// on Node.js v22.17.1
const methods = [
  'log',
  'info',
  'debug',
  'warn',
  'error',
  'dir',
  'time',
  'timeEnd',
  'timeLog',
  'trace',
  'assert',
  'clear',
  'count',
  'countReset',
  'group',
  'groupEnd',
  'table',
  'dirxml',
  'groupCollapsed',
  'Console',
  'profile',
  'profileEnd',
  'timeStamp',
  'context',
  'createTask',
];

const notImplementedMethods = ['context', 'createTask', 'Console'];

export const testMethodNames = {
  async test() {
    for (const method of methods) {
      strictEqual(
        typeof Console[method],
        'function',
        `Method Console.${method} is not a function`
      );
    }
  },
};

export const testNotImplemented = {
  async test() {
    for (const method of notImplementedMethods) {
      throws(() => Console[method](), {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      });
    }
  },
};

export const testMethods = {
  async test() {
    const implementedMethods = methods.filter(
      (m) => !notImplementedMethods.includes(m)
    );
    for (const method of implementedMethods) {
      doesNotThrow(() => Console[method](`Calling Console.${method}()`));
    }
  },
};
