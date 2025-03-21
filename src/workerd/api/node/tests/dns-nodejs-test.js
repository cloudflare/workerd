// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
import dns from 'node:dns';
import dnsPromises from 'node:dns/promises';
import { strictEqual, ok, deepStrictEqual, throws } from 'node:assert';
import { inspect } from 'node:util';

// Taken from Node.js
// https://github.com/nodejs/node/blob/d5d1e80763202ffa73307213211148571deac27c/test/common/internet.js
const addresses = {
  // A generic host that has registered common DNS records,
  // supports both IPv4 and IPv6, and provides basic HTTP/HTTPS services
  INET_HOST: 'nodejs.org',
  // A host that provides IPv4 services
  INET4_HOST: 'nodejs.org',
  // A host that provides IPv6 services
  INET6_HOST: 'nodejs.org',
  // An accessible IPv4 IP,
  // defaults to the Google Public DNS IPv4 address
  INET4_IP: '8.8.8.8',
  // An accessible IPv6 IP,
  // defaults to the Google Public DNS IPv6 address
  INET6_IP: '2001:4860:4860::8888',
  // An invalid host that cannot be resolved
  // See https://tools.ietf.org/html/rfc2606#section-2
  INVALID_HOST: 'something.invalid',
  // A host with MX records registered
  MX_HOST: 'nodejs.org',
  // On some systems, .invalid returns a server failure/try again rather than
  // record not found. Use this to guarantee record not found.
  NOT_FOUND: 'come.on.fhqwhgads.test',
  // A host with SRV records registered
  SRV_HOST: '_caldav._tcp.google.com',
  // A host with PTR records registered
  PTR_HOST: '8.8.8.8.in-addr.arpa',
  // A host with NAPTR records registered
  NAPTR_HOST: 'sip2sip.info',
  // A host with SOA records registered
  SOA_HOST: 'nodejs.org',
  // A host with CAA record registered
  CAA_HOST: 'google.com',
  // A host with CNAME records registered
  CNAME_HOST: 'blog.nodejs.org',
  // A host with NS records registered
  NS_HOST: 'nodejs.org',
  // A host with TXT records registered
  TXT_HOST: 'nodejs.org',
  // An accessible IPv4 DNS server
  DNS4_SERVER: '8.8.8.8',
  // An accessible IPv4 DNS server
  DNS6_SERVER: '2001:4860:4860::8888',
};

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
      'setDefaultResultOrder',
      'getDefaultResultOrder',
      'setServers',
    ];

    for (const fn of syncFns) {
      strictEqual(typeof dns[fn], 'function');
    }

    ok(dns.promises !== undefined);
  },
};

export const errorCodesExist = {
  async test() {
    strictEqual(typeof dns.NODATA, 'string');
    strictEqual(typeof dnsPromises.NODATA, 'string');
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d5d1e80763202ffa73307213211148571deac27c/test/internet/test-dns.js#L483
export const resolveTxt = {
  async test() {
    function validateResult(result) {
      ok(Array.isArray(result[0]));
      strictEqual(result.length, 1);
      ok(result[0][0].startsWith('v=spf1'));
    }

    validateResult(await dnsPromises.resolveTxt(addresses.TXT_HOST));

    {
      // Callback API
      const { promise, resolve, reject } = Promise.withResolvers();
      dns.resolveTxt(addresses.TXT_HOST, (error, results) => {
        if (error) {
          reject(error);
          return;
        }
        validateResult(results);
        resolve();
      });
      await promise;
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d5d1e80763202ffa73307213211148571deac27c/test/internet/test-dns.js#L402
export const resolveCaa = {
  async test() {
    function validateResult(result) {
      ok(Array.isArray(result), `expected array, got ${inspect(result)}`);
      strictEqual(result.length, 1);
      strictEqual(typeof result[0].critical, 'number');
      strictEqual(result[0].critical, 0);
      strictEqual(result[0].issue, 'pki.goog');
    }

    validateResult(await dnsPromises.resolveCaa(addresses.CAA_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d5d1e80763202ffa73307213211148571deac27c/test/internet/test-dns.js#L142
export const resolveMx = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'object');
        ok(item.exchange);
        strictEqual(typeof item.exchange, 'string');
        strictEqual(typeof item.priority, 'number');
      }
    }
    validateResult(await dnsPromises.resolveMx(addresses.MX_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d7fdbb994cda8b2e1da4240eb97270c6abbaa9dd/test/internet/test-dns.js#L442
export const resolveCname = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        ok(item);
        strictEqual(typeof item, 'string');
      }
    }

    validateResult(await dnsPromises.resolveCname(addresses.CNAME_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d7fdbb994cda8b2e1da4240eb97270c6abbaa9dd/test/internet/test-dns.js#L184
export const resolveNs = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        ok(item);
        strictEqual(typeof item, 'string');
      }
    }

    validateResult(await dnsPromises.resolveNs(addresses.NS_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d7fdbb994cda8b2e1da4240eb97270c6abbaa9dd/test/internet/test-dns.js#L268
export const resolvePtr = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        ok(item);
        strictEqual(typeof item, 'string');
      }
    }

    validateResult(await dnsPromises.resolvePtr(addresses.PTR_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d7fdbb994cda8b2e1da4240eb97270c6abbaa9dd/test/internet/test-dns.js#L224
export const resolveSrv = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'object');
        ok(item.name);
        strictEqual(typeof item.name, 'string');
        strictEqual(typeof item.port, 'number');
        strictEqual(typeof item.priority, 'number');
        strictEqual(typeof item.weight, 'number');
      }
    }

    validateResult(await dnsPromises.resolveSrv(addresses.SRV_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d7fdbb994cda8b2e1da4240eb97270c6abbaa9dd/test/internet/test-dns.js#L353
export const resolveSoa = {
  async test() {
    function validateResult(result) {
      strictEqual(typeof result, 'object');
      strictEqual(typeof result.nsname, 'string');
      ok(result.nsname.length > 0);
      strictEqual(typeof result.hostmaster, 'string');
      ok(result.hostmaster.length > 0);
      strictEqual(typeof result.serial, 'number');
      ok(result.serial > 0 && result.serial < 4294967295);
      strictEqual(typeof result.refresh, 'number');
      ok(result.refresh > 0 && result.refresh < 2147483647);
      strictEqual(typeof result.retry, 'number');
      ok(result.retry > 0 && result.retry < 2147483647);
      strictEqual(typeof result.expire, 'number');
      ok(result.expire > 0 && result.expire < 2147483647);
      strictEqual(typeof result.minttl, 'number');
      ok(result.minttl >= 0 && result.minttl < 2147483647);
    }

    validateResult(await dnsPromises.resolveSoa(addresses.SOA_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d7fdbb994cda8b2e1da4240eb97270c6abbaa9dd/test/internet/test-dns.js#L308
export const resolveNaptr = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'object');
        strictEqual(typeof item.flags, 'string');
        strictEqual(typeof item.service, 'string');
        strictEqual(typeof item.regexp, 'string');
        strictEqual(typeof item.replacement, 'string');
        strictEqual(typeof item.order, 'number');
        strictEqual(typeof item.preference, 'number');
      }
    }

    validateResult(await dnsPromises.resolveNaptr(addresses.NAPTR_HOST));
  },
};

export const resolve4 = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'string');
        // TODO(soon): Validate IPv4
        // ok(isIPv4(item));
      }
    }

    validateResult(await dnsPromises.resolve4(addresses.INET4_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d5d1e80763202ffa73307213211148571deac27c/test/internet/test-dns.js#L86
export const resolve4TTL = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'object');
        strictEqual(typeof item.ttl, 'number');
        strictEqual(typeof item.address, 'string');
        ok(item.ttl >= 0);
        // TODO(soon): Validate IPv4
        // ok(isIPv4(item.address));
      }
    }

    validateResult(
      await dnsPromises.resolve4(addresses.INET4_HOST, {
        ttl: true,
      })
    );
  },
};

export const resolve6 = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'string');
        // TODO(soon): Validate IPv6
        // ok(isIPv6(item));
      }
    }

    validateResult(await dnsPromises.resolve6(addresses.INET6_HOST));
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/d5d1e80763202ffa73307213211148571deac27c/test/internet/test-dns.js#L114
export const resolve6TTL = {
  async test() {
    function validateResult(result) {
      ok(result.length > 0);

      for (const item of result) {
        strictEqual(typeof item, 'object');
        strictEqual(typeof item.ttl, 'number');
        strictEqual(typeof item.address, 'string');
        ok(item.ttl >= 0);
        // TODO(soon): Validate IPv6
        // ok(isIPv6(item.address));
      }
    }

    validateResult(
      await dnsPromises.resolve6(addresses.INET6_HOST, {
        ttl: true,
      })
    );
  },
};

export const getServers = {
  async test() {
    deepStrictEqual(await dnsPromises.getServers(), [
      '1.1.1.1',
      '2606:4700:4700::1111',
      '1.0.0.1',
      '2606:4700:4700::1001',
    ]);
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/3153c8333e3a8f2015b795642def4d81ec7cd7b3/test/parallel/test-dns-lookup.js
export const testDnsLookup = {
  async test() {
    {
      const err = {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      };

      throws(() => dns.lookup(1, {}), err);
      throws(() => dnsPromises.lookup(1, {}), err);
    }

    throws(
      () => {
        dns.lookup(false, 'cb');
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      }
    );

    throws(
      () => {
        dns.lookup(false, 'options', 'cb');
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      }
    );

    {
      const family = 20;
      const err = {
        code: 'ERR_INVALID_ARG_VALUE',
        name: 'TypeError',
        message: `The property 'options.family' must be one of: 0, 4, 6. Received ${family}`,
      };
      const options = {
        family,
        all: false,
      };

      throws(() => {
        dnsPromises.lookup(false, options);
      }, err);
      throws(() => {
        dns.lookup(false, options, () => {});
      }, err);
    }

    [1, 0n, 1n, '', '0', Symbol(), true, false, {}, [], () => {}].forEach(
      (family) => {
        const err = { code: 'ERR_INVALID_ARG_VALUE' };
        const options = { family };
        throws(() => {
          dnsPromises.lookup(false, options);
        }, err);
        throws(() => {
          dns.lookup(false, options, () => {});
        }, err);
      }
    );
    [0n, 1n, '', '0', Symbol(), true, false].forEach((family) => {
      const err = { code: 'ERR_INVALID_ARG_TYPE' };
      throws(() => {
        dnsPromises.lookup(false, family);
      }, err);
      throws(() => {
        dns.lookup(false, family, () => {});
      }, err);
    });

    [0, 1, 0n, 1n, '', '0', Symbol(), {}, [], () => {}].forEach((all) => {
      const err = { code: 'ERR_INVALID_ARG_TYPE' };
      const options = { all };
      throws(() => {
        dnsPromises.lookup(false, options);
      }, err);
      throws(() => {
        dns.lookup(false, options, () => {});
      }, err);
    });

    [0, 1, 0n, 1n, '', '0', Symbol(), {}, [], () => {}].forEach((verbatim) => {
      const err = { code: 'ERR_INVALID_ARG_TYPE' };
      const options = { verbatim };
      throws(() => {
        dnsPromises.lookup(false, options);
      }, err);
      throws(() => {
        dns.lookup(false, options, () => {});
      }, err);
    });

    [0, 1, 0n, 1n, '', '0', Symbol(), {}, [], () => {}].forEach((order) => {
      const err = { code: 'ERR_INVALID_ARG_VALUE' };
      const options = { order };
      throws(() => {
        dnsPromises.lookup(false, options);
      }, err);
      throws(() => {
        dns.lookup(false, options, () => {});
      }, err);
    });
  },
};
