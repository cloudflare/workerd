import * as errorCodes from 'node-internal:internal_dns_constants';
import {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  resolveCname,
  resolveNs,
  resolvePtr,
  resolveSrv,
  resolveSoa,
  resolveNaptr,
  resolve4,
  resolve6,
  getServers,
  setServers,
  getDefaultResultOrder,
  setDefaultResultOrder,
  lookup as internalLookup,
  lookupService,
  resolve,
  resolveAny,
  type LookupOptions,
} from 'node-internal:internal_dns';
import {
  CAA,
  MX,
  NAPTR,
  SOA,
  SRV,
  TTLResponse,
} from 'node:internal/internal_dns_client';

export * from 'node-internal:internal_dns_constants';
export {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  resolveCname,
  resolveNs,
  resolvePtr,
  resolveSrv,
  resolveSoa,
  resolveNaptr,
  resolve4,
  resolve6,
  getServers,
  setServers,
  getDefaultResultOrder,
  setDefaultResultOrder,
  lookupService,
  resolve,
  resolveAny,
} from 'node-internal:internal_dns';

export class Resolver {
  // eslint-disable-next-line @typescript-eslint/require-await
  public async cancel(): Promise<void> {
    // TODO(soon): Implement this.
    throw new Error('Not implemented');
  }

  // eslint-disable-next-line @typescript-eslint/require-await
  public async setLocalAddress(): Promise<void> {
    // Does not apply to workerd implementation
    throw new Error('Not implemented');
  }

  public getServers(): Promise<string[]> {
    return getServers();
  }

  public resolve(): Promise<void> {
    return resolve();
  }

  public resolve4(
    input: string,
    options?: { ttl?: boolean }
  ): Promise<(string | TTLResponse)[]> {
    return resolve4(input, options);
  }

  public resolve6(
    input: string,
    options?: { ttl?: boolean }
  ): Promise<(string | TTLResponse)[]> {
    return resolve6(input, options);
  }

  public resolveAny(): Promise<void> {
    return resolveAny();
  }

  public resolveCaa(name: string): Promise<CAA[]> {
    return resolveCaa(name);
  }

  public resolveCname(name: string): Promise<string[]> {
    return resolveCname(name);
  }

  public resolveMx(name: string): Promise<MX[]> {
    return resolveMx(name);
  }

  public resolveNaptr(name: string): Promise<NAPTR[]> {
    return resolveNaptr(name);
  }

  public resolveNs(name: string): Promise<string[]> {
    return resolveNs(name);
  }

  public esolvePtr(name: string): Promise<string[]> {
    return resolvePtr(name);
  }

  public resolveSoa(name: string): Promise<SOA> {
    return resolveSoa(name);
  }

  public resolveSrv(name: string): Promise<SRV[]> {
    return resolveSrv(name);
  }

  public resolveTxt(name: string): Promise<string[][]> {
    return resolveTxt(name);
  }

  public reverse(name: string): Promise<string[]> {
    return reverse(name);
  }

  public setServers(): Promise<void> {
    return setServers();
  }
}

export function lookup(
  hostname: string,
  options?: LookupOptions
): Promise<unknown> {
  const { promise, resolve, reject } = Promise.withResolvers();
  internalLookup(hostname, options, (error, address, family) => {
    if (error) {
      reject(error);
    } else if (Array.isArray(address)) {
      resolve(address);
    } else {
      resolve({
        address,
        family,
      });
    }
  });
  return promise;
}

export default {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  resolveCname,
  resolveNs,
  resolvePtr,
  resolveSrv,
  resolveSoa,
  resolveNaptr,
  resolve4,
  resolve6,
  getServers,
  setServers,
  getDefaultResultOrder,
  setDefaultResultOrder,
  lookup,
  lookupService,
  resolve,
  resolveAny,
  Resolver,
  ...errorCodes,
};
