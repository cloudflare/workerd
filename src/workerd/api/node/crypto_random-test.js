import {
  ok,
  rejects,
  strictEqual,
  throws,
} from 'node:assert';

import {
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
} from 'node:crypto';

import {
  Buffer,
} from 'node:buffer';

function deferredPromise() {
  let resolve, reject;
  const promise = new Promise((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return {
    promise,
    resolve,
    reject,
  }
}

export const test = {
  async test(ctrl, env, ctx) {
    [1, 'hello', {}, []].forEach((i) => {
      throws(() => checkPrimeSync(i), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    for (const checks of [-(2 ** 31), -1, 2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 50]) {
      throws(() => checkPrimeSync(2n, { checks }), {
        code: 'ERR_OUT_OF_RANGE',
        message: /<= 2147483647/
      });
    }

    ok(
      !checkPrimeSync(
        Buffer.from([0x1]),
        {
          fast: true,
          trialDivision: true,
          checks: 10
        }));

    ok(!checkPrimeSync(Buffer.from([0x1])));
    ok(checkPrimeSync(Buffer.from([0x2])));
    ok(checkPrimeSync(Buffer.from([0x3])));
    ok(!checkPrimeSync(Buffer.from([0x4])));

    ////////////////

    ['hello', false, 123].forEach((i) => {
      throws(() => generatePrimeSync(80, i), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    for (const checks of ['hello', {}, []]) {
      throws(() => checkPrimeSync(2n, { checks }), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /checks/
      });
    }

    // //////////////////

    ['hello', false, {}, []].forEach((i) => {
      throws(() => generatePrime(i), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrimeSync(i), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    ['hello', false, 123].forEach((i) => {
      throws(() => generatePrime(80, i, {}), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrimeSync(80, i), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    ['hello', false, 123].forEach((i) => {
      throws(() => generatePrime(80, {}), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    [-1, 0, 2 ** 31, 2 ** 31 + 1, 2 ** 32 - 1, 2 ** 32].forEach((size) => {
      throws(() => generatePrime(-1), {
        code: 'ERR_OUT_OF_RANGE',
        message: />= 1 && <= 2147483647/
      });
      throws(() => generatePrimeSync(size), {
        code: 'ERR_OUT_OF_RANGE',
        message: />= 1 && <= 2147483647/
      });
    });

    // TODO: Fix and enable asynchronous tests
    ['test', -1, {}, []].forEach((i) => {
      throws(() => generatePrime(8, { safe: i }, () => {}), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrime(8, { rem: i }, () => {}), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrime(8, { add: i }, () => {}), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrimeSync(8, { safe: i }), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrimeSync(8, { rem: i }), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
      throws(() => generatePrimeSync(8, { add: i }), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    {
      // Negative BigInts should not be converted to 0 silently.
      throws(() => generatePrime(20, { add: -1n }, () => {}), {
        code: 'ERR_OUT_OF_RANGE'
      });

      throws(() => generatePrime(20, { rem: -1n }, () => {}), {
        code: 'ERR_OUT_OF_RANGE',
      });
      throws(() => checkPrime(-1n, () => {}), {
        code: 'ERR_OUT_OF_RANGE',
      });
    }

    {
      const p = deferredPromise();
      generatePrime(80, (err, prime) => {
        ok(checkPrimeSync(prime));
        checkPrime(prime, (err, result) => {
          ok(result);
          p.resolve();
        });
      });
      await p.promise;
    }

    ok(checkPrimeSync(generatePrimeSync(80)));

    {
      const p = deferredPromise();
      generatePrime(80, {}, (err, prime) => {
        if (err) return p.reject(err);
        ok(checkPrimeSync(prime));
        p.resolve();
      });
      await p.promise;
    }

    ok(checkPrimeSync(generatePrimeSync(80, {})));

    {
      const p = deferredPromise();
      generatePrime(32, { safe: true }, (err, prime) => {
        if (err) return p.reject(err);
        ok(checkPrimeSync(prime));
        const buf = Buffer.from(prime);
        const val = buf.readUInt32BE();
        const check = (val - 1) / 2;
        buf.writeUInt32BE(check);
        ok(checkPrimeSync(buf));
        p.resolve();
      });
      await p.promise;
  }

    {
      const prime = generatePrimeSync(32, { safe: true });
      ok(checkPrimeSync(prime));
      const buf = Buffer.from(prime);
      const val = buf.readUInt32BE();
      const check = (val - 1) / 2;
      buf.writeUInt32BE(check);
      ok(checkPrimeSync(buf));
    }

    const add = 12;
    const rem = 11;
    const add_buf = Buffer.from([add]);
    const rem_buf = Buffer.from([rem]);

    {
      const p = deferredPromise();
      generatePrime(
        32,
        { add: add_buf, rem: rem_buf },
        (err, prime) => {
          if (err) return p.reject(err);
          ok(checkPrimeSync(prime));
          const buf = Buffer.from(prime);
          const val = buf.readUInt32BE();
          strictEqual(val % add, rem);
          p.resolve();
        });
      await p.promise;
    }

    {
      const prime = generatePrimeSync(32, { add: add_buf, rem: rem_buf });
      ok(checkPrimeSync(prime));
      const buf = Buffer.from(prime);
      const val = buf.readUInt32BE();
      strictEqual(val % add, rem);
    }

    {
      const prime = generatePrimeSync(32, { add: BigInt(add), rem: BigInt(rem) });
      ok(checkPrimeSync(prime));
      const buf = Buffer.from(prime);
      const val = buf.readUInt32BE();
      strictEqual(val % add, rem);
    }

    {
      const p = deferredPromise();
      generatePrime(128, {
        bigint: true,
        add: 5n
      }, (err, prime) => {
        // Fails because the add option is not a supported value
        if (err) return p.reject(err);
      });
      await rejects(p.promise);
    }
    {
      const p = deferredPromise();
      generatePrime(128, {
        bigint: true,
        safe: true,
        add: 5n
      }, (err, prime) => {
        // Fails because the add option is not a supported value
        if (err) return p.reject(err);
      });
      await rejects(p.promise);
    }

    // This is impossible because it implies (prime % 2**64) == 1 and
    // prime < 2**64, meaning prime = 1, but 1 is not prime.
    for (const add of [2n ** 64n, 2n ** 65n]) {
      throws(() => {
        generatePrimeSync(64, { add });
      }, {
        name: 'RangeError'
      });
    }

    // Any parameters with rem >= add lead to an impossible condition.
    for (const rem of [7n, 8n, 3000n]) {
      throws(() => {
        generatePrimeSync(64, { add: 7n, rem });
      }, {
        name: 'RangeError'
      });
    }

    // This is possible, but not allowed. It implies prime == 7, which means that
    // we did not actually generate a random prime.
    throws(() => {
      generatePrimeSync(3, { add: 8n, rem: 7n });
    }, {
      name: 'RangeError'
    });

    // We only allow specific values of add and rem
    throws(() => generatePrimeSync(8, {
      add: 7n,
      rem: 1n,
    }), {
      name: 'RangeError'
    });
    throws(() => generatePrimeSync(8, {
      add: 12n,
      rem: 10n,
    }), {
      name: 'RangeError'
    });
    throws(() => generatePrimeSync(8, {
      add: 12n,
    }), {
      name: 'RangeError'
    });

    [1, 'hello', {}, []].forEach((i) => {
      throws(() => checkPrime(i), {
        code: 'ERR_INVALID_ARG_TYPE'
      });
    });

    for (const checks of ['hello', {}, []]) {
      throws(() => checkPrime(2n, { checks }, () => {}), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /checks/
      });
      throws(() => checkPrimeSync(2n, { checks }), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /checks/
      });
    }

    for (const checks of [-(2 ** 31), -1, 2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 50]) {
      throws(() => checkPrime(2n, { checks }, () => {}), {
        code: 'ERR_OUT_OF_RANGE',
        message: /<= 2147483647/
      });
      throws(() => checkPrimeSync(2n, { checks }), {
        code: 'ERR_OUT_OF_RANGE',
        message: /<= 2147483647/
      });
    }

    ok(
      !checkPrimeSync(
        Buffer.from([0x1]),
        {
          fast: true,
          trialDivision: true,
          checks: 10
        }));

    throws(() => {
      generatePrimeSync(32, { bigint: '' });
    }, { code: 'ERR_INVALID_ARG_TYPE' });

    throws(() => {
      generatePrime(32, { bigint: '' }, () => {});
    }, { code: 'ERR_INVALID_ARG_TYPE' });

    {
      const prime = generatePrimeSync(3, { bigint: true });
      strictEqual(typeof prime, 'bigint');
      strictEqual(prime, 7n);
      ok(checkPrimeSync(prime));
      const p = deferredPromise();
      checkPrime(prime, (err, result) => {
        if (err) return p.reject(err);
        p.resolve(result);
      });
      await p.promise
    }

    {
      const p = deferredPromise();
      generatePrime(3, { bigint: true }, (err, prime) => {
        if (err) return p.reject(err);
        strictEqual(typeof prime, 'bigint');
        strictEqual(prime, 7n);
        ok(checkPrimeSync(prime));
        checkPrime(prime, (err, result) => {
          if (err) return p.reject(err);
          p.resolve(result);
        });
      });
      await p.promise;
    }
  }
};
