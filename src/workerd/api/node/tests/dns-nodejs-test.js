import dns from 'node:dns';
import { strictEqual } from 'node:assert';

export const functionsExist = {
  async test() {
    const syncFns = [
      'lookup',
      'lookupService',
      'resolve',
      'resolve4',
      'resolve6',
      'resolveAny',
      'resolveCname',
      'resolveCaa',
      'resolveMx',
      'resolveNaptr',
      'resolveNs',
      'resolvePtr',
      'resolveSoa',
      'resolveSrv',
      'reverse',
      'setDefaultResultOrder',
      'getDefaultResultOrder',
      'setServers',
    ];

    for (const fn of syncFns) {
      strictEqual(typeof dns[fn], 'function');
    }
  },
};
