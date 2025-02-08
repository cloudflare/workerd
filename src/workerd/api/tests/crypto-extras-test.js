import { strictEqual, ok } from 'node:assert';

export const timingSafeEqual = {
  test() {
    // Note that this does not actually test that the equality check is,
    // in fact, timing safe. It checks only the basic operation of the API

    const enc = new TextEncoder();
    [
      [new ArrayBuffer(0), new ArrayBuffer(0)],
      [new ArrayBuffer(1), new ArrayBuffer(1)],
      [enc.encode('hello'), enc.encode('hello')],
      [enc.encode('hellothere'), enc.encode('hellothere').buffer],
    ].forEach(([a, b]) => {
      if (!crypto.subtle.timingSafeEqual(a, b)) {
        throw new Error('inputs should have been equal', a, b);
      }
    });

    [
      [enc.encode('hello'), enc.encode('there')],
      [new Uint8Array([1, 2, 3, 4]), new Uint32Array([1])],
    ].forEach(([a, b]) => {
      if (crypto.subtle.timingSafeEqual(a, b)) {
        throw new Error('inputs should not have been equal', a, b);
      }
    });

    [
      ['hello', 'there'],
      [new ArrayBuffer(0), new ArrayBuffer(1)],
    ].forEach(([a, b]) => {
      try {
        crypto.subtle.timingSafeEqual(a, b);
        throw new Error('inputs should have caused an error', a, b);
      } catch {}
    });
  },
};

export const randomUuid = {
  test() {
    const pattern =
      /[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[ab89][a-f0-9]{3}-[a-f0-9]{12}/;
    // Loop through a bunch of generated UUID's to make sure we're consistently successful.
    for (let n = 0; n < 100; n++) {
      const uuid = crypto.randomUUID();
      if (!pattern.test(uuid)) {
        throw new Error(`${uuid} is not a valid random UUID`);
      }
    }
  },
};

export const cryptoGcmIvZeroLength = {
  async test() {
    const key = await crypto.subtle.generateKey(
      {
        name: 'AES-GCM',
        length: 256,
      },
      true,
      ['encrypt', 'decrypt']
    );

    for (const op of ['encrypt', 'decrypt']) {
      await crypto.subtle[op](
        {
          name: 'AES-GCM',
          iv: new ArrayBuffer(0),
        },
        key,
        new ArrayBuffer(100)
      ).then(
        () => {
          throw new Error('should not have resolved');
        },
        (err) => {
          if (
            err.constructor !== DOMException ||
            err.message !== 'AES-GCM IV must not be empty.'
          ) {
            throw err;
          }
        }
      );
    }
  },
};

export const cryptoZeroLength = {
  async test() {
    function hex2Uint8Array(str) {
      var v = str.match(/.{1,2}/g);
      var buf = new Uint8Array((str.length + 1) / 2);
      for (var i = 0; i < v.length; i++) {
        buf[i] = parseInt(v[i], 16);
      }
      return buf;
    }

    function arrayBuffer2hex(arr) {
      return Array.from(new Uint8Array(arr))
        .map((i) => ('0' + i.toString(16)).slice(-2))
        .join('');
    }

    // Try using a zero-length input to various crypto functions. This should be valid.
    // At one point, encrypt() would sometimes fail on an empty input -- but, mysteriously,
    // it depended on how exactly the ArrayBuffer was constructed! The problem turned out to
    // be BoringSSL rejecting null pointers even if the size was 0.

    const empty = new ArrayBuffer();

    const DIGESTS = {
      MD5: 'd41d8cd98f00b204e9800998ecf8427e',
      'SHA-1': 'da39a3ee5e6b4b0d3255bfef95601890afd80709',
      'SHA-256':
        'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
      'SHA-512':
        'cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e',
    };

    for (const name in DIGESTS) {
      const result = arrayBuffer2hex(await crypto.subtle.digest(name, empty));
      if (result != DIGESTS[name]) {
        throw new Error(
          'for ' + name + ', expected ' + DIGESTS[name] + ' got ' + result
        );
      }
    }

    const ENCRYPTS = {
      'AES-CBC': 'dd3eedef984211b98384dc5677bc728e',
      'AES-GCM': 'fedbd1a722cb7c1a52f529e0469ee449',
    };

    for (const name in ENCRYPTS) {
      const key = await crypto.subtle.importKey(
        'raw',
        new Uint8Array([
          0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0, 1, 2,
          3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
        ]),
        name,
        true,
        ['encrypt']
      );
      const result = arrayBuffer2hex(
        await crypto.subtle.encrypt(
          { name, iv: new Uint8Array(16) },
          key,
          empty
        )
      );
      if (result != ENCRYPTS[name]) {
        throw new Error(
          'for ' + name + ', expected ' + ENCRYPTS[name] + ' got ' + result
        );
      }
    }
  },
};

export const deriveBitsNullLength = {
  async test() {
    // Tests that deriveBits can take a null or undefined length
    // argument and still return the correct number of bits if
    // the algorithm supports it. This is a recent spec change.

    const pair = await crypto.subtle.generateKey(
      {
        name: 'ECDH',
        namedCurve: 'P-384',
      },
      false,
      ['deriveBits']
    );

    {
      const bits = await crypto.subtle.deriveBits(
        {
          name: 'ECDH',
          namedCurve: 'P-384',
          public: pair.publicKey,
        },
        pair.privateKey,
        undefined
      );

      strictEqual(bits.byteLength, 48);
    }

    {
      const bits = await crypto.subtle.deriveBits(
        {
          name: 'ECDH',
          namedCurve: 'P-384',
          public: pair.publicKey,
        },
        pair.privateKey,
        null
      );

      strictEqual(bits.byteLength, 48);
    }

    {
      const bits = await crypto.subtle.deriveBits(
        {
          name: 'ECDH',
          namedCurve: 'P-384',
          public: pair.publicKey,
        },
        pair.privateKey
      );

      strictEqual(bits.byteLength, 48);
    }
  },
};

export const aesCounterOverflowTest = {
  async test() {
    // Regression test: Check that the input counter is not modified when it overflows in the
    // internal computation.
    const key = await crypto.subtle.generateKey(
      {
        name: 'AES-CTR',
        length: 128,
      },
      false,
      ['encrypt']
    );

    // Maximum counter value, will overflow and require processing in two parts if there is more
    // than one input data block.
    const counter = new Uint8Array([
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255,
    ]);
    const counter2 = counter.slice();

    await crypto.subtle.encrypt(
      {
        name: 'AES-CTR',
        length: 128,
        counter,
      },
      key,
      new TextEncoder().encode('A'.repeat(2 * 16))
    );
    ok(crypto.subtle.timingSafeEqual(counter, counter2));
  },
};
