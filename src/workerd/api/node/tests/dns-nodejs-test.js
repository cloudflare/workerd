import dns from 'node:dns';
import { strictEqual } from 'node:assert';

export const functionsExist = {
  async test() {
    strictEqual(typeof dns.lookup, 'function');
    strictEqual(typeof dns.lookupService, 'function');
    strictEqual(typeof dns.resolve, 'function');
    strictEqual(typeof dns.resolve4, 'function');
    strictEqual(typeof dns.resolve6, 'function');
    strictEqual(typeof dns.resolveAny, 'function');
    strictEqual(typeof dns.resolveCname, 'function');
    strictEqual(typeof dns.resolveCaa, 'function');
    strictEqual(typeof dns.resolveMx, 'function');
    strictEqual(typeof dns.resolveNaptr, 'function');
    strictEqual(typeof dns.resolveNs, 'function');
    strictEqual(typeof dns.resolvePtr, 'function');
    strictEqual(typeof dns.resolveSoa, 'function');
    strictEqual(typeof dns.resolveSrv, 'function');
    strictEqual(typeof dns.resolveTxt, 'function');
    strictEqual(typeof dns.reverse, 'function');
    strictEqual(typeof dns.setDefaultResultOrder, 'function');
    strictEqual(typeof dns.getDefaultResultOrder, 'function');
    strictEqual(typeof dns.setServers, 'function');
  },
};
