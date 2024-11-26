import dns from 'node:dns';
import dnsPromises from 'node:dns/promises';
import { strictEqual, throws } from 'node:assert';

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

    for (const module of [dns, dnsPromises]) {
      throws(
        () => {
          const resolver = new module.Resolver();
          resolver.cancel();
        },
        { message: 'Not implemented' }
      );
    }
  },
};

export const errorCodesExist = {
  async test() {
    strictEqual(typeof dns.NODATA, 'string');
    strictEqual(typeof dnsPromises.NODATA, 'string');
  },
};
