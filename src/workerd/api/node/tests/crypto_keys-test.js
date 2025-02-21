import {
  notDeepStrictEqual,
  deepStrictEqual,
  strictEqual,
  ok,
  rejects,
  throws,
} from 'node:assert';
import {
  KeyObject,
  SecretKeyObject,
  PrivateKeyObject,
  createSecretKey,
  createPrivateKey,
  createPublicKey,
  createHmac,
  generateKey,
  generateKeySync,
  generateKeyPair,
  generateKeyPairSync,
  generatePrimeSync,
  diffieHellman,
} from 'node:crypto';
import { Buffer } from 'node:buffer';
import { promisify } from 'node:util';

export const secret_key_equals_test = {
  async test() {
    const secretKeyData1 = Buffer.from('abcdefghijklmnop');
    const secretKeyData2 = Buffer.from('abcdefghijklmnop'.repeat(2));
    const aes1 = await crypto.subtle.importKey(
      'raw',
      secretKeyData1,
      { name: 'AES-CBC' },
      true,
      ['encrypt', 'decrypt']
    );
    const aes2 = await crypto.subtle.importKey(
      'raw',
      secretKeyData1,
      { name: 'AES-CBC' },
      true,
      ['encrypt', 'decrypt']
    );
    const aes3 = await crypto.subtle.importKey(
      'raw',
      secretKeyData2,
      { name: 'AES-CBC' },
      true,
      ['encrypt', 'decrypt']
    );
    const hmac = await crypto.subtle.importKey(
      'raw',
      secretKeyData1,
      {
        name: 'HMAC',
        hash: 'SHA-256',
      },
      true,
      ['sign', 'verify']
    );
    const hmac2 = await crypto.subtle.importKey(
      'raw',
      secretKeyData1,
      {
        name: 'HMAC',
        hash: 'SHA-256',
      },
      false,
      ['sign', 'verify']
    );

    const aes1_ko = KeyObject.from(aes1);
    const aes2_ko = KeyObject.from(aes2);
    const aes3_ko = KeyObject.from(aes3);
    const hmac_ko = KeyObject.from(hmac);
    const hmac2_ko = KeyObject.from(hmac2);

    strictEqual(aes1_ko.type, 'secret');
    strictEqual(aes2_ko.type, 'secret');
    strictEqual(aes3_ko.type, 'secret');
    strictEqual(hmac_ko.type, 'secret');

    ok(aes1_ko.equals(aes1_ko));
    ok(aes1_ko.equals(aes2_ko));
    ok(aes1_ko.equals(hmac_ko));
    ok(hmac_ko.equals(aes1_ko));
    ok(hmac_ko.equals(aes2_ko));

    ok(!aes1_ko.equals(aes3_ko));
    ok(!hmac_ko.equals(aes3_ko));

    // Unable to determine equality if either key is not extractable.
    ok(!hmac_ko.equals(hmac2_ko));
    ok(!hmac2_ko.equals(hmac_ko));
  },
};

// In case anyone comes across these tests and wonders why there are
// keys embedded... these are test keys only. They are not sensitive
// or secret in any way. They are used only in the test here and are
// pulled directly from MDN examples.

const jwkEcKey = {
  crv: 'P-384',
  d: 'wouCtU7Nw4E8_7n5C1-xBjB4xqSb_liZhYMsy8MGgxUny6Q8NCoH9xSiviwLFfK_',
  ext: true,
  key_ops: ['sign'],
  kty: 'EC',
  x: 'SzrRXmyI8VWFJg1dPUNbFcc9jZvjZEfH7ulKI1UkXAltd7RGWrcfFxqyGPcwu6AQ',
  y: 'hHUag3OvDzEr0uUQND4PXHQTXP5IDGdYhJhL-WLKjnGjQAw0rNGy5V29-aV-yseW',
};

const pemEncodedKey = `
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAy3Xo3U13dc+xojwQYWoJLCbOQ5fOVY8LlnqcJm1W1BFtxIhOAJWohiHuIRMctv7d
zx47TLlmARSKvTRjd0dF92jx/xY20Lz+DXp8YL5yUWAFgA3XkO3LSJ
gEOex10NB8jfkmgSb7QIudTVvbbUDfd5fwIBmCtaCwWx7NyeWWDb7A
9cFxj7EjRdrDaK3ux/ToMLHFXVLqSL341TkCf4ZQoz96RFPUGPPLOf
vN0x66CM1PQCkdhzjE6U5XGE964ZkkYUPPsy6Dcie4obhW4vDjgUmL
zv0z7UD010RLIneUgDE2FqBfY/C+uWigNPBPkkQ+Bv/UigS6dHqTCV
eD5wgyBQIDAQAB
`;

export const asymmetric_key_equals_test = {
  async test() {
    const jwk1_ec = await crypto.subtle.importKey(
      'jwk',
      jwkEcKey,
      {
        name: 'ECDSA',
        namedCurve: 'P-384',
      },
      true,
      ['sign']
    );
    const jwk2_ec = await crypto.subtle.importKey(
      'jwk',
      jwkEcKey,
      {
        name: 'ECDSA',
        namedCurve: 'P-384',
      },
      true,
      ['sign']
    );
    strictEqual(jwk1_ec.type, 'private');

    // fetch the part of the PEM string between header and footer
    const pemContent = Buffer.from(pemEncodedKey, 'base64');

    const rsa = await crypto.subtle.importKey(
      'spki',
      pemContent,
      {
        name: 'RSA-OAEP',
        hash: 'SHA-256',
      },
      true,
      ['encrypt']
    );
    const rsa2 = await crypto.subtle.importKey(
      'spki',
      pemContent,
      {
        name: 'RSA-OAEP',
        hash: 'SHA-256',
      },
      false,
      ['encrypt']
    );
    strictEqual(rsa.type, 'public');

    const jwk1_ko = KeyObject.from(jwk1_ec);
    const jwk2_ko = KeyObject.from(jwk2_ec);
    const rsa_ko = KeyObject.from(rsa);
    const rsa2_ko = KeyObject.from(rsa2);

    strictEqual(jwk1_ko.asymmetricKeyType, 'ec');
    strictEqual(rsa_ko.asymmetricKeyType, 'rsa');
    strictEqual(rsa_ko.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(rsa_ko.asymmetricKeyDetails.publicExponent, 65537n);
    strictEqual(jwk1_ko.asymmetricKeyDetails.namedCurve, 'secp384r1');

    ok(jwk1_ko.equals(jwk1_ko));
    ok(jwk1_ko.equals(jwk2_ko));
    ok(!rsa_ko.equals(jwk1_ko));
    ok(!jwk1_ko.equals(rsa_ko));
    ok(!rsa_ko.equals(rsa2_ko));

    jwk1_ko.export({
      type: 'pkcs8',
      format: 'der',
      cipher: 'aes-128-cbc',
      passphrase: Buffer.alloc(0),
    });
    jwk1_ko.export({
      type: 'pkcs8',
      format: 'der',
      cipher: 'aes-128-cbc',
      passphrase: '',
    });
  },
};

export const secret_key_test = {
  test() {
    const buf = Buffer.from('hello');
    strictEqual(buf.toString(), 'hello');
    const key1 = createSecretKey('hello');
    const key2 = createSecretKey(buf);
    const key3 = createSecretKey('there');

    // Creating a zero-length key should also just work.
    const key4 = createSecretKey('');
    const key5 = createSecretKey(Buffer.alloc(0));
    ok(key4.equals(key5));
    strictEqual(key4.export().toString(), '');

    // The key should be immutable after creation,
    // so modifying the buf now should have no impact
    // on the test.
    buf.fill(0);
    strictEqual(buf.toString(), '\0\0\0\0\0');

    // Now let's make sure the keys we created meet our expectations.
    strictEqual(key1.export().toString(), 'hello');
    strictEqual(key2.export().toString(), 'hello');
    strictEqual(key3.export({ format: 'buffer' }).toString(), 'there');

    strictEqual(key1.toString(), '[object KeyObject]');
    strictEqual(key2.toString(), '[object KeyObject]');
    strictEqual(key3.toString(), '[object KeyObject]');
    ok(key1 instanceof SecretKeyObject);
    ok(key2 instanceof SecretKeyObject);
    ok(key3 instanceof SecretKeyObject);
    strictEqual(key1.type, 'secret');
    strictEqual(key2.type, 'secret');
    strictEqual(key3.type, 'secret');
    ok(key1.equals(key2));
    ok(!key1.equals(key3));
    ok(!key3.equals(key1));

    strictEqual(key1.symmetricKeySize, 5);

    const jwk = key3.export({ format: 'jwk' });
    ok(jwk);
    strictEqual(typeof jwk, 'object');
    strictEqual(jwk.kty, 'oct');
    strictEqual(jwk.ext, true);
    strictEqual(jwk.k, 'dGhlcmU');
  },
};

export const create_private_key_test_generic = {
  test(_, env) {
    // TODO(later): These error messages are inconsistent with one another
    // despite performing the same basic validation.
    throws(() => createPrivateKey(1), {
      message: /The \"options.key\" property must be of type string/,
    });
    throws(() => createPrivateKey(true), {
      message: /The \"options.key\" property must be of type string/,
    });
    throws(() => createPrivateKey({ key: 1 }), {
      message: /The \"key\" argument/,
    });
    throws(() => createPrivateKey({ key: true }), {
      message: /The \"key\" argument/,
    });

    // The "bad" cases here are just the ones that we expect to fail
    // creating as private keys, generally because boringssl does not
    // support these variations.
    [
      'dh_private.pem',
      'ec_secp256k1_private.pem',
      'ed448_private.pem',
      'rsa_pss_private_2048.pem',
      'rsa_pss_private_2048_sha1_sha1_20.pem',
      'rsa_pss_private_2048_sha256_sha256_16.pem',
      'rsa_pss_private_2048_sha512_sha256_20.pem',
      'x448_private.pem',
    ].forEach((i) => {
      throws(() => createPrivateKey(env[i]), {
        message: 'Failed to parse private key',
      });
    });

    // These next three are encrypted and require a passphrase.
    createPrivateKey({
      key: env['rsa_private_encrypted.pem'],
      passphrase: 'password',
    });

    // Trying to parse an encrypted private key without specifying a
    // passphrase, or specifying the wrong passphrase, should fail.
    throws(
      () => {
        createPrivateKey({
          key: env['rsa_private_encrypted.pem'],
          passphrase: 'password_bad',
        });
      },
      { message: 'Failed to parse private key' }
    );

    throws(
      () => {
        createPrivateKey({
          key: env['rsa_private_encrypted.pem'],
        });
      },
      { message: 'Failed to parse private key' }
    );

    createPrivateKey({
      key: env['dsa_private_encrypted.pem'],
      passphrase: 'password',
    });

    createPrivateKey({
      key: env['dsa_private_encrypted_1025.pem'],
      passphrase: 'secret',
    });

    [
      'dsa_private.pem',
      'dsa_private_1025.pem',
      'dsa_private_pkcs8.pem',
      'ed25519_private.pem',
      'ec_p256_private.pem',
      'ec_p384_private.pem',
      'ec_p521_private.pem',
      'rsa_private.pem',
      'rsa_private_b.pem',
      'rsa_private_2048.pem',
      'rsa_private_pkcs8_bad.pem',
      'rsa_private_4096.pem',
      'rsa_private_pkcs8.pem',
      'x25519_private.pem',
    ].forEach((i) => {
      try {
        const key = createPrivateKey(env[i]);

        // These variations should also just work.
        const buf = Buffer.from(env[i]);
        createPrivateKey({ key: env[i] });
        createPrivateKey(buf);
        createPrivateKey(buf.buffer);
        createPrivateKey(new DataView(buf.buffer));
        createPrivateKey({ key: buf });
        createPrivateKey({ key: buf.buffer });
        createPrivateKey({ key: new DataView(buf.buffer) });

        ok(key instanceof PrivateKeyObject);

        const details = key.asymmetricKeyDetails;
        switch (key.asymmetricKeyType) {
          case 'dsa': {
            switch (i) {
              case 'dsa_private.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.divisorLength, 256);
                break;
              }
              case 'dsa_private_1025.pem': {
                strictEqual(details.modulusLength, 1088);
                strictEqual(details.divisorLength, 160);
                break;
              }
              case 'dsa_private_pkcs8.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.divisorLength, 256);
                break;
              }
            }
            break;
          }
          case 'rsa': {
            switch (i) {
              case 'rsa_private.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.publicExponent, 65537n);
                break;
              }
              case 'rsa_private_b.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.publicExponent, 65537n);
                break;
              }
              case 'rsa_private_2048.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.publicExponent, 65537n);
                break;
              }
              case 'rsa_private_pkcs8_bad.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.publicExponent, 65537n);
                break;
              }
              case 'rsa_private_4096.pem': {
                strictEqual(details.modulusLength, 4096);
                strictEqual(details.publicExponent, 65537n);
                break;
              }
              case 'rsa_private_pkcs8.pem': {
                strictEqual(details.modulusLength, 2048);
                strictEqual(details.publicExponent, 65537n);
                break;
              }
            }
            break;
          }
          case 'ed25519': {
            break;
          }
          case 'x25519': {
            break;
          }
          case 'ec': {
            switch (i) {
              case 'ec_p256_private.pem': {
                strictEqual(details.namedCurve, 'prime256v1');
                break;
              }
              case 'ec_p384_private.pem': {
                strictEqual(details.namedCurve, 'secp384r1');
                break;
              }
              case 'ec_p521_private.pem': {
                strictEqual(details.namedCurve, 'secp521r1');
                break;
              }
            }
            break;
          }
          default:
            throw new Error('Invalid key type');
        }

        // Export/Import roundtrip should work
        {
          const exported = key.export({ format: 'pem', type: 'pkcs8' });
          const key2 = createPrivateKey(exported);
          const exported2 = key2.export({ format: 'pem', type: 'pkcs8' });
          strictEqual(exported.toString(), exported2.toString());
          ok(key.equals(key2));
        }
        {
          const exported = key.export({ format: 'der', type: 'pkcs8' });
          const key2 = createPrivateKey({ key: exported, format: 'der' });
          const exported2 = key2.export({ format: 'der', type: 'pkcs8' });
          strictEqual(exported.toString(), exported2.toString());
          ok(key.equals(key2));
        }

        if (key.asymmetricKeyType == 'ec') {
          const exported = key.export({ format: 'der', type: 'sec1' });
          const key2 = createPrivateKey({
            key: exported,
            format: 'der',
            type: 'sec1',
          });
          const exported2 = key2.export({ format: 'der', type: 'sec1' });
          strictEqual(exported.toString(), exported2.toString());
        } else {
          throws(() => key.export({ format: 'der', type: 'sec1' }), {
            message: 'SEC1 can only be used for EC keys',
          });
        }
      } catch (err) {
        console.log(`${i} FAILED`, err.message);
      }
    });
  },
};

export const private_key_import_type = {
  test() {
    // This is a DER-encoded PKCS8 ed25519 private key. In this case, just like
    // with Node.js, the specific "type" argument ends up being ignored.
    // The actual behavior in Node.js is a bit inconsistent and depends on
    // the type of key. For instance, for RSA keys, the type argument is
    // respected and it is not always possible, for instance, to import a
    // PKCS1 exported key as PKCS8, etc. The edges cases here appear to be
    // undocumented in Node.js and may not be fully tested in Node.js either.
    const derData = Buffer.from([
      0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70,
      0x04, 0x22, 0x04, 0x20, 0xc1, 0x52, 0xba, 0x33, 0x74, 0x8c, 0x85, 0x08,
      0x77, 0x34, 0xaf, 0xbb, 0x19, 0x1a, 0xd2, 0x57, 0xe0, 0x55, 0x59, 0x0c,
      0x75, 0x14, 0xee, 0x69, 0x5b, 0xc8, 0x61, 0x41, 0xeb, 0xbf, 0x35, 0xd0,
    ]);

    // All three of these variations should work, depsite the type being different.
    const k1 = createPrivateKey({ key: derData, format: 'der', type: 'pkcs1' });
    const k2 = createPrivateKey({ key: derData, format: 'der', type: 'pkcs8' });
    const k3 = createPrivateKey({ key: derData, format: 'der', type: 'sec1' });

    const t1 = k1.export({ format: 'pem', type: 'pkcs8' });
    const t2 = k2.export({ format: 'pem', type: 'pkcs8' });
    const t3 = k3.export({ format: 'pem', type: 'pkcs8' });
    strictEqual(t1.toString(), t2.toString());
    strictEqual(t1.toString(), t3.toString());

    ok(k1.equals(k2));
    ok(k2.equals(k3));
    ok(k3.equals(k1));
  },
};

export const private_key_import_rsa_der = {
  test() {
    // A DER-encoded RSA PKCS8 private key
    const derData = Buffer.from(
      '30820942020100300d06092a864886f70d01010105000482092c3082092802' +
        '01000282020100c5e4adc287db8ed66ea25e25c0a9f5c34f00cdc48d2c77c2' +
        'a8d1476be5bb7abf22b3b2d0b0f963b4f4f8ca7e25b1aa3cde780716f482de' +
        'd6cc891343a541d8a87db006e524b8e982b589192e2d37d3b1f40b071557a5' +
        'b93f7b4d12df34209f8faa5471b539277b352d6ce6ac6c88bf6a8ae75561be' +
        'a6c82a0c6ac5678a13b16b621344af8bbb03a8de44d9d8a3987fd8627967c2' +
        'ab338331ddb409c0469d5b1b7e04eb8b2a58298ba0609f3dd1edca6fc249dc' +
        '4e2ceb03575e63af0383650fd3259e6d864c0a651c6eee0f5fc25840d4f888' +
        'e560cfe2b79735aa9ad9b4a0af3a40963404c58b383742fba24495389417f4' +
        '111aa16b6af0d2d4f0ca4c746a073435345ebeb42dfaf9f19e6a9fc44612f9' +
        '53c6809b3c9ccf576ce0cc3fe5984a9fb7a3677d3725bde037b1df314ab381' +
        '527e28dd2f214b46fd4aeecf06e9078ccf7afded117a822c852c179504cbaf' +
        '616df0ac2ea723e6d65b4d7130ed8ea1dc21653a060fa43f2f66c5256d1866' +
        '5800fb6a1b334103f9bea0e8dafce48fc406e53dd967ebdd43683b99b399ee' +
        '6421b019c2da256822ca8aaf5c07325c3645c87e0e65987caec04b29b9c55c' +
        '767fce21b4700a3313281910f25aa69c3c2f1fda772ed62ff4898f1a2888a6' +
        'a973b0677f2f1e48b577f9700ac612d6fcc43a0a127e7bff04c40634d9c6b8' +
        '3290afab0224f537d5e8533ca561aa1f34058f4bf734bd0203010001028202' +
        '00536fc7936d94b4f4d450c14149aa5f64a9babd07523e9d80058db77f56ad' +
        '6563914e12e6cab75bc2c046e599aa6aee4c1bc09fbc9dfb4fd96103aa8baa' +
        'f1c857c226a5c1976a1f8a6ce0112dd702e2cef50671461e5e516ce29fec85' +
        '0f8571c1311fc9918f37864b358be4f66e0c7a2881c867c77e8af37a4721fd' +
        '795a4e534fe35a1c6ba78e824c80eaa6af20cafb9c5068bfc6e44823d8b291' +
        '664b1b7add1f0a5328bcd46db796975825cbfae737a34757bcfb7914dda3c8' +
        'b85ee22c544007d6a4a5a92a0677fb350a4a91256ff065db245d12249482b3' +
        'ce7cd02d5a6b25767a24da69e8a07a63526aa650245a669672e18348ebf17a' +
        'f869afdc9bbfb9b4af1f3a3140b96f814a9602756c862bc5d5324898748888' +
        'b22f04a3e9dbd483ea6696213d430bdb0a0b813582aed53b8efada5d7fff31' +
        '0e018ed72b467b2eb6d93b85971e7b2014ea46b6143783d422fd278cea66f9' +
        'c442530540539a4846b35c7c8e897371adf49aabfc4fadd6e0f5e45c799974' +
        'aeefc87e6eb1e888b3478651e6982bcf765a07f1081a36e0908d42201c1ffc' +
        '13c15e25e3244ea9e1f65784a16c86260b395afbeeefb0f616840c1047d787' +
        'd3ee2881391a310ae6d7aa65664d1ecd55fd04a2bc2939dde06f7f4f5f15b2' +
        'd41b8a8c05dbf70a9c62823aaae6a11daf1ce92c41d5095669f0091a94d6cc' +
        'a6071858d06488a6cf7cbfe954604c4a410282010100fee558136fc778d715' +
        'fee58023d2eca403571c9137dd314c67c4aec8e62da77318262ce6435b93ec' +
        'e9b62c46a70e7362cfe7c0421ac4be8d6c0c817dc4e01d7b6aa3207fa20f37' +
        '0bbc53e00b782442ca458d4df533d38ee71023cc1c5f2f7283886d8cb664d1' +
        '84db8a257e7a7823aa904a7a58a696380df971c68a607c4dccc4b8f70782bf' +
        'de593e947dab4275b20cc3ec7b55878cae5c8d35fc10bc174466623b6cb94a' +
        '3e3e607ab0ed8af78baaaa8d063d4a2cecc860adb7dfdcbdfa12438f826462' +
        '035015672d6c28ac542aca464379b57e0f8203ff86646724bd8915a2dd6516' +
        'bc26ad2d9da24f04853d4771525d7befbe8f1ef326c44b80546f2875e49702' +
        '82010100c6c01fc484238e32a6bb2242cc8bf11bb967bb3a2f772e0e11d4ae' +
        '24be322b99b7e011c91e7fb21de44872b0e724a5135187df707fce8ce9c94b' +
        'de0103ac07d6d12ebc6433e8b57a529c00a59d4cff699d9cfd1e0b62f626d4' +
        '912fc99dc5fcaa5c396858ca43eeb9dc059e8284b119eac5986ba581413011' +
        '98f55e3f252934d1b6ab29b1377fef44118606e9c3de3eb35f4334b3358ae6' +
        '253eaac603cb72b308a1bc154ed7c70cd16cfee2011443e47a6d666cb65b9d' +
        '84260d6e8669555c9326df648405fac3da80e69678c0a9a65daa2e4cab67b8' +
        'b81ddd84cf51c09a694802c6058ea4f4379a9c015b3a56b0309ae2da993b07' +
        '4a610a7f83376a60d3e5b7cb028201005e4f0cdf64243199a311c4683cd8f5' +
        'a5597709a2d1408dd4ef2fde5b868eadbdefd970136228a7faa81e37138d0b' +
        'd3b563a7238351d4298cb9c586c3b9ec11fc6fe01b4e1deff335ec603c2d02' +
        '2ea8679e8441abcf991eee6f124f9acfbd06699438b42f67edfd721d12f250' +
        'edd284710e9d65df7d05106692aa1ad8c82520f648595df60a77821d9d6341' +
        'd23d29bb7f6227dfe55f2fc41e9b32c01e579d7f24294878e5f751acf0b835' +
        'ab8d1ba7f1a26c0491453df6858ec0d19b22cf3ba2b39e52f5d0b3f8b74c1f' +
        '108d7236c2d06c76c3a7f8a4ea45c8bbad4df2b29dc6bc93826deb01783732' +
        'ae79c5b27e94771d0f960cb377880f77e15781e5feda5fd1028201007f3514' +
        'a018db10f64654dbd6d94870678841664a157b3854f500a4fd0b66dd1523e5' +
        '1c3d17722fb4861a009e4d32dd1d023feeb8f874612879183fdd725637263c' +
        'f8a6c79399cc1da0a60c9bf394069db8ad742c38a97c56da129afd7627f451' +
        'ad7968d9fb8b834e1e0ed2a742fa7f560e6641efca4cc8d15a8f216555098c' +
        'aef5359417c327f52221fd208b9a3bb2f1e7750253f95f0f72a32b7655936f' +
        'b43b40193ba21ce55fc4e2f837faecd78f72f4766bfa43a50ba1b75318606e' +
        'ac33dadb7c602bdb966351c14469c116544efacf6b6f0191eef5de84549544' +
        'ab0fdb713b00ef8d9069ce612f550e7fd1812a812bdc8b355d5bc2f65e2ba7' +
        'c0959f20050282010100baf8445f5361adfb0889993acfaf27c14644a33387' +
        '5114c0ed8fbee213be44edbbfec4e088f2c40a397640289245900860017228' +
        '83249118516a9c517e39a900a13a0b66354da877a7fdb60c83171379292f1c' +
        '137d8d5d5deb257b376c166620a2d23c44dfdee20fb84a453e3a5d479bf98e' +
        '557e4c38c7e11ac8b44de10c2bf2a535e15d73265a9dd57d86e011f20fd37b' +
        '91da22212fefe0dc152a6ebcb03c4b1356248d34cfee0434326a65cb5c1d5b' +
        '43623f39be3523cd8f0f588d0aa1a2ed9bd9dde5581a0513432c3a4383bc82' +
        '54cf3fd702485557c04f601db2def13a9f4d9096acce9cac65fa05bea1576b' +
        '6a36c03eec9ebd53d346bb0e4636a5e369f5',
      'hex'
    );

    // Just like with Node.js, it is possible to import a PKCS8 private key (in this case an
    // rsa key) using any of pkcs1, pkcs8, or sec1 as the type value.
    const k1 = createPrivateKey({ key: derData, format: 'der', type: 'pkcs1' });
    const k2 = createPrivateKey({ key: derData, format: 'der', type: 'pkcs8' });
    const k3 = createPrivateKey({ key: derData, format: 'der', type: 'sec1' });
    ok(k1.equals(k2));
    ok(k1.equals(k3));
    ok(k3.equals(k2));

    // A DER-encoded RSA PKCS1 private key
    const derData2 = Buffer.from(
      '308209280201000282020100c5e4adc287db8ed66ea25e25c0a9f5c34f' +
        '00cdc48d2c77c2a8d1476be5bb7abf22b3b2d0b0f963b4f4f8ca7e25b1' +
        'aa3cde780716f482ded6cc891343a541d8a87db006e524b8e982b58919' +
        '2e2d37d3b1f40b071557a5b93f7b4d12df34209f8faa5471b539277b35' +
        '2d6ce6ac6c88bf6a8ae75561bea6c82a0c6ac5678a13b16b621344af8b' +
        'bb03a8de44d9d8a3987fd8627967c2ab338331ddb409c0469d5b1b7e04' +
        'eb8b2a58298ba0609f3dd1edca6fc249dc4e2ceb03575e63af0383650f' +
        'd3259e6d864c0a651c6eee0f5fc25840d4f888e560cfe2b79735aa9ad9' +
        'b4a0af3a40963404c58b383742fba24495389417f4111aa16b6af0d2d4' +
        'f0ca4c746a073435345ebeb42dfaf9f19e6a9fc44612f953c6809b3c9c' +
        'cf576ce0cc3fe5984a9fb7a3677d3725bde037b1df314ab381527e28dd' +
        '2f214b46fd4aeecf06e9078ccf7afded117a822c852c179504cbaf616d' +
        'f0ac2ea723e6d65b4d7130ed8ea1dc21653a060fa43f2f66c5256d1866' +
        '5800fb6a1b334103f9bea0e8dafce48fc406e53dd967ebdd43683b99b3' +
        '99ee6421b019c2da256822ca8aaf5c07325c3645c87e0e65987caec04b' +
        '29b9c55c767fce21b4700a3313281910f25aa69c3c2f1fda772ed62ff4' +
        '898f1a2888a6a973b0677f2f1e48b577f9700ac612d6fcc43a0a127e7b' +
        'ff04c40634d9c6b83290afab0224f537d5e8533ca561aa1f34058f4bf7' +
        '34bd020301000102820200536fc7936d94b4f4d450c14149aa5f64a9ba' +
        'bd07523e9d80058db77f56ad6563914e12e6cab75bc2c046e599aa6aee' +
        '4c1bc09fbc9dfb4fd96103aa8baaf1c857c226a5c1976a1f8a6ce0112d' +
        'd702e2cef50671461e5e516ce29fec850f8571c1311fc9918f37864b35' +
        '8be4f66e0c7a2881c867c77e8af37a4721fd795a4e534fe35a1c6ba78e' +
        '824c80eaa6af20cafb9c5068bfc6e44823d8b291664b1b7add1f0a5328' +
        'bcd46db796975825cbfae737a34757bcfb7914dda3c8b85ee22c544007' +
        'd6a4a5a92a0677fb350a4a91256ff065db245d12249482b3ce7cd02d5a' +
        '6b25767a24da69e8a07a63526aa650245a669672e18348ebf17af869af' +
        'dc9bbfb9b4af1f3a3140b96f814a9602756c862bc5d5324898748888b2' +
        '2f04a3e9dbd483ea6696213d430bdb0a0b813582aed53b8efada5d7fff' +
        '310e018ed72b467b2eb6d93b85971e7b2014ea46b6143783d422fd278c' +
        'ea66f9c442530540539a4846b35c7c8e897371adf49aabfc4fadd6e0f5' +
        'e45c799974aeefc87e6eb1e888b3478651e6982bcf765a07f1081a36e0' +
        '908d42201c1ffc13c15e25e3244ea9e1f65784a16c86260b395afbeeef' +
        'b0f616840c1047d787d3ee2881391a310ae6d7aa65664d1ecd55fd04a2' +
        'bc2939dde06f7f4f5f15b2d41b8a8c05dbf70a9c62823aaae6a11daf1c' +
        'e92c41d5095669f0091a94d6cca6071858d06488a6cf7cbfe954604c4a' +
        '410282010100fee558136fc778d715fee58023d2eca403571c9137dd31' +
        '4c67c4aec8e62da77318262ce6435b93ece9b62c46a70e7362cfe7c042' +
        '1ac4be8d6c0c817dc4e01d7b6aa3207fa20f370bbc53e00b782442ca45' +
        '8d4df533d38ee71023cc1c5f2f7283886d8cb664d184db8a257e7a7823' +
        'aa904a7a58a696380df971c68a607c4dccc4b8f70782bfde593e947dab' +
        '4275b20cc3ec7b55878cae5c8d35fc10bc174466623b6cb94a3e3e607a' +
        'b0ed8af78baaaa8d063d4a2cecc860adb7dfdcbdfa12438f8264620350' +
        '15672d6c28ac542aca464379b57e0f8203ff86646724bd8915a2dd6516' +
        'bc26ad2d9da24f04853d4771525d7befbe8f1ef326c44b80546f2875e4' +
        '970282010100c6c01fc484238e32a6bb2242cc8bf11bb967bb3a2f772e' +
        '0e11d4ae24be322b99b7e011c91e7fb21de44872b0e724a5135187df70' +
        '7fce8ce9c94bde0103ac07d6d12ebc6433e8b57a529c00a59d4cff699d' +
        '9cfd1e0b62f626d4912fc99dc5fcaa5c396858ca43eeb9dc059e8284b1' +
        '19eac5986ba58141301198f55e3f252934d1b6ab29b1377fef44118606' +
        'e9c3de3eb35f4334b3358ae6253eaac603cb72b308a1bc154ed7c70cd1' +
        '6cfee2011443e47a6d666cb65b9d84260d6e8669555c9326df648405fa' +
        'c3da80e69678c0a9a65daa2e4cab67b8b81ddd84cf51c09a694802c605' +
        '8ea4f4379a9c015b3a56b0309ae2da993b074a610a7f83376a60d3e5b7' +
        'cb028201005e4f0cdf64243199a311c4683cd8f5a5597709a2d1408dd4' +
        'ef2fde5b868eadbdefd970136228a7faa81e37138d0bd3b563a7238351' +
        'd4298cb9c586c3b9ec11fc6fe01b4e1deff335ec603c2d022ea8679e84' +
        '41abcf991eee6f124f9acfbd06699438b42f67edfd721d12f250edd284' +
        '710e9d65df7d05106692aa1ad8c82520f648595df60a77821d9d6341d2' +
        '3d29bb7f6227dfe55f2fc41e9b32c01e579d7f24294878e5f751acf0b8' +
        '35ab8d1ba7f1a26c0491453df6858ec0d19b22cf3ba2b39e52f5d0b3f8' +
        'b74c1f108d7236c2d06c76c3a7f8a4ea45c8bbad4df2b29dc6bc93826d' +
        'eb01783732ae79c5b27e94771d0f960cb377880f77e15781e5feda5fd1' +
        '028201007f3514a018db10f64654dbd6d94870678841664a157b3854f5' +
        '00a4fd0b66dd1523e51c3d17722fb4861a009e4d32dd1d023feeb8f874' +
        '612879183fdd725637263cf8a6c79399cc1da0a60c9bf394069db8ad74' +
        '2c38a97c56da129afd7627f451ad7968d9fb8b834e1e0ed2a742fa7f56' +
        '0e6641efca4cc8d15a8f216555098caef5359417c327f52221fd208b9a' +
        '3bb2f1e7750253f95f0f72a32b7655936fb43b40193ba21ce55fc4e2f8' +
        '37faecd78f72f4766bfa43a50ba1b75318606eac33dadb7c602bdb9663' +
        '51c14469c116544efacf6b6f0191eef5de84549544ab0fdb713b00ef8d' +
        '9069ce612f550e7fd1812a812bdc8b355d5bc2f65e2ba7c0959f200502' +
        '82010100baf8445f5361adfb0889993acfaf27c14644a333875114c0ed' +
        '8fbee213be44edbbfec4e088f2c40a3976402892459008600172288324' +
        '9118516a9c517e39a900a13a0b66354da877a7fdb60c83171379292f1c' +
        '137d8d5d5deb257b376c166620a2d23c44dfdee20fb84a453e3a5d479b' +
        'f98e557e4c38c7e11ac8b44de10c2bf2a535e15d73265a9dd57d86e011' +
        'f20fd37b91da22212fefe0dc152a6ebcb03c4b1356248d34cfee043432' +
        '6a65cb5c1d5b43623f39be3523cd8f0f588d0aa1a2ed9bd9dde5581a05' +
        '13432c3a4383bc8254cf3fd702485557c04f601db2def13a9f4d9096ac' +
        'ce9cac65fa05bea1576b6a36c03eec9ebd53d346bb0e4636a5e369f5',
      'hex'
    );

    // However, just like with Node.js, when the der data is exported as pkcs1, trying to read
    // it as pkcs8 fails... tho oddly sec1 is ok. Silly software.
    const k4 = createPrivateKey({
      key: derData2,
      format: 'der',
      type: 'pkcs1',
    });
    const k6 = createPrivateKey({ key: derData2, format: 'der', type: 'sec1' });

    ok(k4.equals(k6));

    throws(
      () => createPrivateKey({ key: derData2, format: 'der', type: 'pkcs8' }),
      {
        message: 'Failed to parse private key',
      }
    );
  },
};

export const private_key_jwk_export = {
  test(_, env) {
    // These are invalid for JWK export
    [
      'dsa_private.pem',
      'dsa_private_1025.pem',
      'dsa_private_pkcs8.pem',
    ].forEach((i) => {
      throws(() => createPrivateKey(env[i]).export({ format: 'jwk' }), {
        message: 'Key type is invalid for JWK export',
      });
    });

    deepStrictEqual(
      createPrivateKey(env['ed25519_private.pem']).export({ format: 'jwk' }),
      {
        alg: 'EdDSA',
        crv: 'Ed25519',
        d: 'wVK6M3SMhQh3NK-7GRrSV-BVWQx1FO5pW8hhQeu_NdA',
        kty: 'OKP',
        x: 'K1wIouqnuiA04b3WrMa-xKIKIpfHetNZRv3h9fBf768',
      }
    );

    deepStrictEqual(
      createPrivateKey(env['x25519_private.pem']).export({ format: 'jwk' }),
      {
        crv: 'X25519',
        d: 'mL_IWm55RrALUGRfJYzw40gEYWMvtRkesP9mj8o8Omc',
        kty: 'OKP',
        x: 'aSb8Q-RndwfNnPeOYGYPDUN3uhAPnMLzXyfi-mqfhig',
      }
    );

    deepStrictEqual(
      createPrivateKey(env['ec_p256_private.pem']).export({ format: 'jwk' }),
      {
        crv: 'P-256',
        d: 'DxBsPQPIgMuMyQbxzbb9toew6Ev6e9O6ZhpxLNgmAEo',
        kty: 'EC',
        x: 'X0mMYR_uleZSIPjNztIkAS3_ud5LhNpbiIFp6fNf2Gs',
        y: 'UbJuPy2Xi0lW7UYTBxPK3yGgDu9EAKYIecjkHX5s2lI',
      }
    );

    deepStrictEqual(
      createPrivateKey(env['ec_p384_private.pem']).export({ format: 'jwk' }),
      {
        crv: 'P-384',
        d: 'dwfuHuAtTlMRn7ZBCBm_0grpc1D_4hPeNAgevgelljuC0--k_LDFosDgBlLLmZsi',
        kty: 'EC',
        x: 'hON3nzGJgv-08fdHpQxgRJFZzlK-GZDGa5f3KnvM31cvvjJmsj4UeOgIdy3rDAjV',
        y: 'fidHhtecNCGCfLqmrLjDena1NSzWzWH1u_oUdMKGo5XSabxzD7-8JZxjpc8sR9cl',
      }
    );

    deepStrictEqual(
      createPrivateKey(env['ec_p521_private.pem']).export({ format: 'jwk' }),
      {
        crv: 'P-521',
        d:
          'ABIIbmn3Gm_Y11uIDkC3g2ijpRxIrJEBY4i_JJYo5OougzTl3BX2ifRluP' +
          'JMaaHcNerbQH_WdVkLLX86ShlHrRyJ',
        kty: 'EC',
        x:
          'AaLFgjwZtznM3N7qsfb86awVXe6c6djUYOob1FN-kllekv0KEXV0bwcDjPGQ' +
          'z5f6MxLCbhMeHRavUS6P10rsTtBn',
        y:
          'Ad3flexBeAfXceNzRBH128kFbOWD6W41NjwKRqqIF26vmgW_8COldGKZjFkO' +
          'SEASxPBcvA2iFJRUyQ3whC00j0Np',
      }
    );

    deepStrictEqual(
      createPrivateKey(env['rsa_private.pem']).export({ format: 'jwk' }),
      {
        d:
          'ktnq2LvIMqBj4txP82IEOorIRQGVsw1khbm8A-cEpuEkgM71Yi_0WzupKktuc' +
          'UeevQ5i0Yh8w9e1SJiTLDRAlJz66kdky9uejiWWl6zR4dyNZVMFYRM43ijLC-' +
          'P8rPne9Fz16IqHFW5VbJqA1xCBhKmuPMsD71RNxZ4Hrsa7Kt_xglQTYsLbdGI' +
          'wDmcZihId9VGXRzvmCPsDRf2fCkAj7HDeRxpUdEiEDpajADc-PWikra3r3b40' +
          'tVHKWm8wxJLivOIN7GiYXKQIW6RhZgH-Rk45JIRNKxNagxdeXUqqyhnwhbTo1' +
          'Hite0iBDexN9tgoZk0XmdYWBn6ElXHRZ7VCDQ',
        dp:
          'qS_Mdr5CMRGGMH0bKhPUWEtAixUGZhJaunX5wY71Xoc_Gh4cnO-b7BNJ_-5L' +
          '8WZog0vr6PgiLhrqBaCYm2wjpyoG2o2wDHm-NAlzN_wp3G2EFhrSxdOux-S1' +
          'c0kpRcyoiAO2n29rNDa-jOzwBBcU8ACEPdLOCQl0IEFFJO33tl8',
        dq:
          'WAziKpxLKL7LnL4dzDcx8JIPIuwnTxh0plCDdCffyLaT8WJ9lXbXHFTjOvt8' +
          'WfPrlDP_Ylxmfkw5BbGZOP1VLGjZn2DkH9aMiwNmbDXFPdG0G3hzQovx_9fa' +
          'jiRV4DWghLHeT9wzJfZabRRiI0VQR472300AVEeX4vgbrDBn600',
        e: 'AQAB',
        kty: 'RSA',
        n:
          't9xYiIonscC3vz_A2ceR7KhZZlDu_5bye53nCVTcKnWd2seY6UAdKersX6njr' +
          '83Dd5OVe1BW_wJvp5EjWTAGYbFswlNmeD44edEGM939B6Lq-_8iBkrTi8mGN4' +
          'YCytivE24YI0D4XZMPfkLSpab2y_Hy4DjQKBq1ThZ0UBnK-9IhX37Ju_ZoGYS' +
          'lTIGIhzyaiYBh7wrZBoPczIEu6et_kN2VnnbRUtkYTF97ggcv5h-hDpUQjQW0' +
          'ZgOMcTc8n-RkGpIt0_iM_bTjI3Tz_gsFdi6hHcpZgbopPL630296iByyigQCP' +
          'JVzdusFrQN5DeC-zT_nGypQkZanLb4ZspSx9Q',
        p:
          '8UovlB4nrBm7xH-u7XXBMbqxADQm5vaEZxw9eluc-tP7cIAI4sglMIvL_FMpb' +
          'd2pEeP_BkR76NTDzzDuPAZvUGRavgEjy0O9j2NAs_WPK4tZF-vFdunhnSh4EH' +
          'AF4Ij9kbsUi90NOpbGfVqPdOaHqzgHKoR23Cuusk9wFQ2XTV8',
        q:
          'wxHdEYT9xrpfrHPqSBQPpO0dWGKJEkrWOb-76rSfuL8wGR4OBNmQdhLuU9zTI' +
          'h22pog-XPnLPAecC-4yu_wtJ2SPCKiKDbJBre0CKPyRfGqzvA3njXwMxXazU4' +
          'kGs-2Fg-xu_iKbaIjxXrclBLhkxhBtySrwAFhxxOk6fFcPLSs',
        qi:
          'k7czBCT9rHn_PNwCa17hlTy88C4vXkwbz83Oa-aX5L4e5gw5lhcR2ZuZHLb2' +
          'r6oMt9rlD7EIDItSs-u21LOXWPTAlazdnpYUyw_CzogM_PN-qNwMRXn5uXFF' +
          'hmlP2mVg2EdELTahXch8kWqHaCSX53yvqCtRKu_j76V31TfQZGM',
      }
    );
  },
};

export const rsa_private_key_jwk_import = {
  test() {
    const jwk = {
      e: 'AQAB',
      n:
        't9xYiIonscC3vz_A2ceR7KhZZlDu_5bye53nCVTcKnWd2seY6UAdKersX6njr83Dd5OVe' +
        '1BW_wJvp5EjWTAGYbFswlNmeD44edEGM939B6Lq-_8iBkrTi8mGN4YCytivE24YI0D4XZ' +
        'MPfkLSpab2y_Hy4DjQKBq1ThZ0UBnK-9IhX37Ju_ZoGYSlTIGIhzyaiYBh7wrZBoPczIE' +
        'u6et_kN2VnnbRUtkYTF97ggcv5h-hDpUQjQW0ZgOMcTc8n-RkGpIt0_iM_bTjI3Tz_gsF' +
        'di6hHcpZgbopPL630296iByyigQCPJVzdusFrQN5DeC-zT_nGypQkZanLb4ZspSx9Q',
      d:
        'ktnq2LvIMqBj4txP82IEOorIRQGVsw1khbm8A-cEpuEkgM71Yi_0WzupKktucUeevQ5i0' +
        'Yh8w9e1SJiTLDRAlJz66kdky9uejiWWl6zR4dyNZVMFYRM43ijLC-P8rPne9Fz16IqHFW' +
        '5VbJqA1xCBhKmuPMsD71RNxZ4Hrsa7Kt_xglQTYsLbdGIwDmcZihId9VGXRzvmCPsDRf2' +
        'fCkAj7HDeRxpUdEiEDpajADc-PWikra3r3b40tVHKWm8wxJLivOIN7GiYXKQIW6RhZgH-' +
        'Rk45JIRNKxNagxdeXUqqyhnwhbTo1Hite0iBDexN9tgoZk0XmdYWBn6ElXHRZ7VCDQ',
      p:
        '8UovlB4nrBm7xH-u7XXBMbqxADQm5vaEZxw9eluc-tP7cIAI4sglMIvL_FMpbd2pEeP_B' +
        'kR76NTDzzDuPAZvUGRavgEjy0O9j2NAs_WPK4tZF-vFdunhnSh4EHAF4Ij9kbsUi90NOp' +
        'bGfVqPdOaHqzgHKoR23Cuusk9wFQ2XTV8',
      q:
        'wxHdEYT9xrpfrHPqSBQPpO0dWGKJEkrWOb-76rSfuL8wGR4OBNmQdhLuU9zTIh22pog-X' +
        'PnLPAecC-4yu_wtJ2SPCKiKDbJBre0CKPyRfGqzvA3njXwMxXazU4kGs-2Fg-xu_iKbaI' +
        'jxXrclBLhkxhBtySrwAFhxxOk6fFcPLSs',
      dp:
        'qS_Mdr5CMRGGMH0bKhPUWEtAixUGZhJaunX5wY71Xoc_Gh4cnO-b7BNJ_-5L8WZog0vr' +
        '6PgiLhrqBaCYm2wjpyoG2o2wDHm-NAlzN_wp3G2EFhrSxdOux-S1c0kpRcyoiAO2n29rN' +
        'Da-jOzwBBcU8ACEPdLOCQl0IEFFJO33tl8',
      dq:
        'WAziKpxLKL7LnL4dzDcx8JIPIuwnTxh0plCDdCffyLaT8WJ9lXbXHFTjOvt8WfPrlDP_' +
        'Ylxmfkw5BbGZOP1VLGjZn2DkH9aMiwNmbDXFPdG0G3hzQovx_9fajiRV4DWghLHeT9wzJ' +
        'fZabRRiI0VQR472300AVEeX4vgbrDBn600',
      qi:
        'k7czBCT9rHn_PNwCa17hlTy88C4vXkwbz83Oa-aX5L4e5gw5lhcR2ZuZHLb2r6oMt9rl' +
        'D7EIDItSs-u21LOXWPTAlazdnpYUyw_CzogM_PN-qNwMRXn5uXFFhmlP2mVg2EdELTahX' +
        'ch8kWqHaCSX53yvqCtRKu_j76V31TfQZGM',
      kty: 'RSA',
    };

    const key = createPrivateKey({ key: jwk, format: 'jwk' });
    strictEqual(key.asymmetricKeyType, 'rsa');
    deepStrictEqual(key.asymmetricKeyDetails, {
      modulusLength: 2048,
      publicExponent: 65537n,
    });

    // The fail due to missing information in the JWK
    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'RSA',
          },
          format: 'jwk',
        });
      },
      {
        message: 'RSA JWK missing n parameter',
      }
    );

    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'RSA',
            n:
              't9xYiIonscC3vz_A2ceR7KhZZlDu_5bye53nCVTcKnWd2seY6UAdKersX6njr83Dd5OVe' +
              '1BW_wJvp5EjWTAGYbFswlNmeD44edEGM939B6Lq-_8iBkrTi8mGN4YCytivE24YI0D4XZ' +
              'MPfkLSpab2y_Hy4DjQKBq1ThZ0UBnK-9IhX37Ju_ZoGYSlTIGIhzyaiYBh7wrZBoPczIE' +
              'u6et_kN2VnnbRUtkYTF97ggcv5h-hDpUQjQW0ZgOMcTc8n-RkGpIt0_iM_bTjI3Tz_gsF' +
              'di6hHcpZgbopPL630296iByyigQCPJVzdusFrQN5DeC-zT_nGypQkZanLb4ZspSx9Q',
          },
          format: 'jwk',
        });
      },
      {
        message: 'RSA JWK missing e parameter',
      }
    );

    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'RSA',
            n:
              't9xYiIonscC3vz_A2ceR7KhZZlDu_5bye53nCVTcKnWd2seY6UAdKersX6njr83Dd5OVe' +
              '1BW_wJvp5EjWTAGYbFswlNmeD44edEGM939B6Lq-_8iBkrTi8mGN4YCytivE24YI0D4XZ' +
              'MPfkLSpab2y_Hy4DjQKBq1ThZ0UBnK-9IhX37Ju_ZoGYSlTIGIhzyaiYBh7wrZBoPczIE' +
              'u6et_kN2VnnbRUtkYTF97ggcv5h-hDpUQjQW0ZgOMcTc8n-RkGpIt0_iM_bTjI3Tz_gsF' +
              'di6hHcpZgbopPL630296iByyigQCPJVzdusFrQN5DeC-zT_nGypQkZanLb4ZspSx9Q',
            e: 'AQAB',
          },
          format: 'jwk',
        });
      },
      {
        message: 'RSA JWK missing d parameter',
      }
    );
  },
};

export const ec_private_key_jwk_import = {
  test() {
    const jwk = {
      crv: 'P-256',
      d: 'DxBsPQPIgMuMyQbxzbb9toew6Ev6e9O6ZhpxLNgmAEo',
      kty: 'EC',
      x: 'X0mMYR_uleZSIPjNztIkAS3_ud5LhNpbiIFp6fNf2Gs',
      y: 'UbJuPy2Xi0lW7UYTBxPK3yGgDu9EAKYIecjkHX5s2lI',
    };

    const key = createPrivateKey({ key: jwk, format: 'jwk' });
    strictEqual(key.asymmetricKeyType, 'ec');
    deepStrictEqual(key.asymmetricKeyDetails, { namedCurve: 'prime256v1' });

    // The fail due to missing information in the JWK
    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'EC',
          },
          format: 'jwk',
        });
      },
      {
        message: 'EC JWK missing crv parameter',
      }
    );

    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'EC',
            crv: 'P-256',
          },
          format: 'jwk',
        });
      },
      {
        message: 'EC JWK missing x parameter',
      }
    );

    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'EC',
            crv: 'P-256',
            x: 'X0mMYR_uleZSIPjNztIkAS3_ud5LhNpbiIFp6fNf2Gs',
          },
          format: 'jwk',
        });
      },
      {
        message: 'EC JWK missing y parameter',
      }
    );
  },
};

export const ed_private_key_jwk_import = {
  test() {
    const jwk = {
      crv: 'Ed25519',
      x: 'K1wIouqnuiA04b3WrMa-xKIKIpfHetNZRv3h9fBf768',
      d: 'wVK6M3SMhQh3NK-7GRrSV-BVWQx1FO5pW8hhQeu_NdA',
      kty: 'OKP',
    };

    const key = createPrivateKey({ key: jwk, format: 'jwk' });
    strictEqual(key.asymmetricKeyType, 'ed25519');

    const jwk2 = {
      crv: 'X25519',
      x: 'K1wIouqnuiA04b3WrMa-xKIKIpfHetNZRv3h9fBf768',
      d: 'wVK6M3SMhQh3NK-7GRrSV-BVWQx1FO5pW8hhQeu_NdA',
      kty: 'OKP',
    };

    const key2 = createPrivateKey({ key: jwk2, format: 'jwk' });
    strictEqual(key2.asymmetricKeyType, 'x25519');

    // The fail due to missing information in the JWK
    throws(
      () => {
        createPrivateKey({
          key: {
            kty: 'OKP',
          },
          format: 'jwk',
        });
      },
      {
        message: 'OKP JWK missing crv parameter',
      }
    );
  },
};

export const private_key_hmac_fails = {
  test(_, env) {
    // Verify that creating an hmac with a private key fails.
    // TODO(soon): Also try public key, which should also fail.
    const key = createPrivateKey(env['dsa_private.pem']);
    throws(() => createHmac('sha256', key), {
      message: 'Invalid key object type private, expected secret.',
    });
  },
};

export const private_key_tocryptokey = {
  async test(_, env) {
    const key = createPrivateKey(env['dsa_private.pem']);
    // TODO(soon): Getting a CryptoKey from the KeyObject currently does not work.
    // Will will implement this conversion as a follow up step.
    throws(() => key.toCryptoKey(), {
      message: 'The toCryptoKey method is not implemented',
    });

    // Getting the private key from a CryptoKey should generally work...
    const { privateKey } = await crypto.subtle.generateKey(
      {
        name: 'RSASSA-PKCS1-v1_5',
        modulusLength: 2048,
        publicExponent: new Uint8Array([1, 0, 1]),
        hash: 'SHA-256',
      },
      true,
      ['sign', 'verify']
    );

    const key2 = KeyObject.from(privateKey);
    strictEqual(key2.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(key2.asymmetricKeyDetails.publicExponent, 65537n);
    strictEqual(key2.asymmetricKeyType, 'rsa');
    strictEqual(key2.type, 'private');
  },
};

export const create_public_key = {
  test(_, env) {
    // TODO(later): These error messages are inconsistent with one another
    // despite performing the same basic validation.
    throws(() => createPublicKey(1), {
      message: /The \"options.key\" property must be of type string/,
    });
    throws(() => createPublicKey(true), {
      message: /The \"options.key\" property must be of type string/,
    });
    throws(() => createPublicKey({ key: 1 }), {
      message: /The \"key\" argument/,
    });
    throws(() => createPublicKey({ key: true }), {
      message: /The \"key\" argument/,
    });

    [
      // These are the public key types that are not supported by boringssl
      'dh_public.pem',
      'ec_secp256k1_public.pem',
      'ed448_public.pem',
      'rsa_pss_public_2048.pem',
      'rsa_pss_public_2048_sha1_sha1_20.pem',
      'rsa_pss_public_2048_sha256_sha256_16.pem',
      'rsa_pss_public_2048_sha512_sha256_20.pem',
      'x448_public.pem',
    ].forEach((i) => {
      throws(() => createPublicKey(env[i]), {
        message: 'Failed to parse public key',
      });
    });

    [
      'dsa_public_1025.pem',
      'dsa_public.pem',
      'ec_p256_public.pem',
      'ec_p384_public.pem',
      'ec_p521_public.pem',
      'ed25519_public.pem',
      'rsa_public_2048.pem',
      'rsa_public_4096.pem',
      'rsa_public_b.pem',
      'rsa_public.pem',
      'x25519_public.pem',

      // It is also possible to use an X509 cert as the source of a public key
      'agent1-cert.pem',

      // It is possible to create a public key from a private key
      'dsa_private.pem',
      'dsa_private_1025.pem',
      'dsa_private_pkcs8.pem',
      'ed25519_private.pem',
      'ec_p256_private.pem',
      'ec_p384_private.pem',
      'ec_p521_private.pem',
      'rsa_private.pem',
      'rsa_private_b.pem',
      'rsa_private_2048.pem',
      'rsa_private_pkcs8_bad.pem',
      'rsa_private_4096.pem',
      'rsa_private_pkcs8.pem',
      'x25519_private.pem',
    ].forEach((i) => {
      const key = createPublicKey(env[i]);
      strictEqual(key.type, 'public');

      switch (i) {
        case 'dsa_public_1025.pem': {
          strictEqual(key.asymmetricKeyType, 'dsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 1088);
          strictEqual(key.asymmetricKeyDetails.divisorLength, 160);
          break;
        }
        case 'dsa_public.pem': {
          strictEqual(key.asymmetricKeyType, 'dsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.divisorLength, 256);
          break;
        }
        case 'ec_p256_public.pem': {
          strictEqual(key.asymmetricKeyType, 'ec');
          strictEqual(key.asymmetricKeyDetails.namedCurve, 'prime256v1');
          break;
        }
        case 'ec_p384_public.pem': {
          strictEqual(key.asymmetricKeyType, 'ec');
          strictEqual(key.asymmetricKeyDetails.namedCurve, 'secp384r1');
          break;
        }
        case 'ec_p521_public.pem': {
          strictEqual(key.asymmetricKeyType, 'ec');
          strictEqual(key.asymmetricKeyDetails.namedCurve, 'secp521r1');
          break;
        }
        case 'ed25519_public.pem': {
          strictEqual(key.asymmetricKeyType, 'ed25519');
          break;
        }
        case 'rsa_public_2048.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_public_4096.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 4096);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_public_b.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_public.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'x25519_public.pem': {
          strictEqual(key.asymmetricKeyType, 'x25519');
          break;
        }
        case 'dsa_private.pem': {
          strictEqual(key.asymmetricKeyType, 'dsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.divisorLength, 256);
          break;
        }
        case 'dsa_private_1025.pem': {
          strictEqual(key.asymmetricKeyType, 'dsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 1088);
          strictEqual(key.asymmetricKeyDetails.divisorLength, 160);
          break;
        }
        case 'dsa_private_pkcs8.pem': {
          strictEqual(key.asymmetricKeyType, 'dsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.divisorLength, 256);
          break;
        }
        case 'ed25519_private.pem': {
          strictEqual(key.asymmetricKeyType, 'ed25519');
          break;
        }
        case 'ec_p256_private.pem': {
          strictEqual(key.asymmetricKeyType, 'ec');
          strictEqual(key.asymmetricKeyDetails.namedCurve, 'prime256v1');
          break;
        }
        case 'ec_p384_private.pem': {
          strictEqual(key.asymmetricKeyType, 'ec');
          strictEqual(key.asymmetricKeyDetails.namedCurve, 'secp384r1');
          break;
        }
        case 'ec_p521_private.pem': {
          strictEqual(key.asymmetricKeyType, 'ec');
          strictEqual(key.asymmetricKeyDetails.namedCurve, 'secp521r1');
          break;
        }
        case 'rsa_private.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_private_b.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_private_2048.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_private_pkcs8_bad.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_private_4096.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 4096);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'rsa_private_pkcs8.pem': {
          strictEqual(key.asymmetricKeyType, 'rsa');
          strictEqual(key.asymmetricKeyDetails.modulusLength, 2048);
          strictEqual(key.asymmetricKeyDetails.publicExponent, 65537n);
          break;
        }
        case 'x25519_private.pem': {
          strictEqual(key.asymmetricKeyType, 'x25519');
          break;
        }
      }

      if (key.asymmetricKeyType === 'rsa') {
        const exp = key.export({ format: 'pem', type: 'pkcs1' });
        const key2 = createPublicKey(exp);
        ok(key.equals(key2));

        const exp2 = key.export({ format: 'der', type: 'pkcs1' });
        const key3 = createPublicKey({
          key: exp2,
          format: 'der',
          type: 'pkcs1',
        });
        ok(key.equals(key3));
      }
      {
        const exp = key.export({ format: 'pem', type: 'spki' });
        const key2 = createPublicKey(exp);
        ok(key.equals(key2));
      }
      {
        const exp = key.export({ format: 'der', type: 'spki' });
        const key2 = createPublicKey({ key: exp, format: 'der', type: 'spki' });
        ok(key.equals(key2));
      }
    });

    const privateKey = createPrivateKey(env['rsa_private.pem']);
    const publicKey = createPublicKey(privateKey);
    strictEqual(publicKey.type, 'public');
    strictEqual(publicKey.asymmetricKeyType, 'rsa');
    strictEqual(publicKey.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(publicKey.asymmetricKeyDetails.publicExponent, 65537n);
  },
};

export const public_key_tocryptokey = {
  async test(_, env) {
    const key = createPublicKey(env['dsa_public_1025.pem']);
    // TODO(soon): Getting a CryptoKey from the KeyObject currently does not work.
    // Will will implement this conversion as a follow up step.
    throws(() => key.toCryptoKey(), {
      message: 'The toCryptoKey method is not implemented',
    });

    // Getting the private key from a CryptoKey should generally work...
    const { publicKey } = await crypto.subtle.generateKey(
      {
        name: 'RSASSA-PKCS1-v1_5',
        modulusLength: 2048,
        publicExponent: new Uint8Array([1, 0, 1]),
        hash: 'SHA-256',
      },
      true,
      ['sign', 'verify']
    );

    const key2 = KeyObject.from(publicKey);
    strictEqual(key2.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(key2.asymmetricKeyDetails.publicExponent, 65537n);
    strictEqual(key2.asymmetricKeyType, 'rsa');
    strictEqual(key2.type, 'public');
  },
};

export const export_public_key_jwk = {
  test(_, env) {
    // These are invalid for JWK export
    [
      'dsa_private.pem',
      'dsa_private_1025.pem',
      'dsa_private_pkcs8.pem',
    ].forEach((i) => {
      throws(() => createPrivateKey(env[i]).export({ format: 'jwk' }), {
        message: 'Key type is invalid for JWK export',
      });
    });

    deepStrictEqual(
      createPublicKey(env['ec_p256_public.pem']).export({ format: 'jwk' }),
      {
        crv: 'P-256',
        kty: 'EC',
        x: 'X0mMYR_uleZSIPjNztIkAS3_ud5LhNpbiIFp6fNf2Gs',
        y: 'UbJuPy2Xi0lW7UYTBxPK3yGgDu9EAKYIecjkHX5s2lI',
      }
    );

    deepStrictEqual(
      createPublicKey(env['ec_p384_public.pem']).export({ format: 'jwk' }),
      {
        crv: 'P-384',
        kty: 'EC',
        x: 'hON3nzGJgv-08fdHpQxgRJFZzlK-GZDGa5f3KnvM31cvvjJmsj4UeOgIdy3rDAjV',
        y: 'fidHhtecNCGCfLqmrLjDena1NSzWzWH1u_oUdMKGo5XSabxzD7-8JZxjpc8sR9cl',
      }
    );

    deepStrictEqual(
      createPublicKey(env['ec_p521_public.pem']).export({ format: 'jwk' }),
      {
        crv: 'P-521',
        kty: 'EC',
        x:
          'AaLFgjwZtznM3N7qsfb86awVXe6c6djUYOob1FN-kllekv0KEXV0bwcDjPGQz5f' +
          '6MxLCbhMeHRavUS6P10rsTtBn',
        y:
          'Ad3flexBeAfXceNzRBH128kFbOWD6W41NjwKRqqIF26vmgW_8COldGKZjFkOSEA' +
          'SxPBcvA2iFJRUyQ3whC00j0Np',
      }
    );

    deepStrictEqual(
      createPublicKey(env['ed25519_public.pem']).export({ format: 'jwk' }),
      {
        alg: 'EdDSA',
        crv: 'Ed25519',
        kty: 'OKP',
        x: 'K1wIouqnuiA04b3WrMa-xKIKIpfHetNZRv3h9fBf768',
      }
    );

    deepStrictEqual(
      createPublicKey(env['x25519_public.pem']).export({ format: 'jwk' }),
      {
        crv: 'X25519',
        kty: 'OKP',
        x: 'aSb8Q-RndwfNnPeOYGYPDUN3uhAPnMLzXyfi-mqfhig',
      }
    );

    deepStrictEqual(
      createPublicKey(env['rsa_public_2048.pem']).export({ format: 'jwk' }),
      {
        e: 'AQAB',
        kty: 'RSA',
        n:
          'rk4OqxBqU5_k0FoUDU7CpZpjz6YJEXUpyqeJmFRVZPMUv_Rc7U4seLY-Qp6k26' +
          'T_wlQ2WJWuyY-VJcbQNWLvjJWks5HWknwDuVs6sjuTM8CfHWn1960JkK5Ec2Tj' +
          'RhCQ1KJy-uc3GJLtWb4rWVgTbbaaC5fiR1_GeuJ8JH1Q50lB3mDsNGIk1U5jhN' +
          'aYY82hYvlbErf6Ft5njHK0BOM5OTvQ6BBv7c363WNG7tYlNw1J40dup9OQPo5J' +
          'mXN_h-sRbdgG8iUxrkRibuGv7loh52QQgq2snznuRMdKidRfUZjCDGgwbgK23Q' +
          '7n8VZ9Y10j8PIvPTLJ83PX4lOEA37Jlw',
      }
    );

    // TODO(now): JWK import
    throws(() => createPublicKey({ key: {}, format: 'jwk' }), {
      message: 'JWK public key import is not implemented for this key type',
    });
  },
};

export const rsa_public_key_jwk_import = {
  test() {
    const jwk = {
      e: 'AQAB',
      n:
        't9xYiIonscC3vz_A2ceR7KhZZlDu_5bye53nCVTcKnWd2seY6UAdKersX6njr83Dd5OVe' +
        '1BW_wJvp5EjWTAGYbFswlNmeD44edEGM939B6Lq-_8iBkrTi8mGN4YCytivE24YI0D4XZ' +
        'MPfkLSpab2y_Hy4DjQKBq1ThZ0UBnK-9IhX37Ju_ZoGYSlTIGIhzyaiYBh7wrZBoPczIE' +
        'u6et_kN2VnnbRUtkYTF97ggcv5h-hDpUQjQW0ZgOMcTc8n-RkGpIt0_iM_bTjI3Tz_gsF' +
        'di6hHcpZgbopPL630296iByyigQCPJVzdusFrQN5DeC-zT_nGypQkZanLb4ZspSx9Q',
      kty: 'RSA',
    };

    const key = createPublicKey({ key: jwk, format: 'jwk' });
    strictEqual(key.asymmetricKeyType, 'rsa');
    deepStrictEqual(key.asymmetricKeyDetails, {
      modulusLength: 2048,
      publicExponent: 65537n,
    });

    // The fail due to missing information in the JWK
    throws(
      () => {
        createPublicKey({
          key: {
            kty: 'RSA',
          },
          format: 'jwk',
        });
      },
      {
        message: 'RSA JWK missing n parameter',
      }
    );

    throws(
      () => {
        createPublicKey({
          key: {
            kty: 'RSA',
            n:
              't9xYiIonscC3vz_A2ceR7KhZZlDu_5bye53nCVTcKnWd2seY6UAdKersX6njr83Dd5OVe' +
              '1BW_wJvp5EjWTAGYbFswlNmeD44edEGM939B6Lq-_8iBkrTi8mGN4YCytivE24YI0D4XZ' +
              'MPfkLSpab2y_Hy4DjQKBq1ThZ0UBnK-9IhX37Ju_ZoGYSlTIGIhzyaiYBh7wrZBoPczIE' +
              'u6et_kN2VnnbRUtkYTF97ggcv5h-hDpUQjQW0ZgOMcTc8n-RkGpIt0_iM_bTjI3Tz_gsF' +
              'di6hHcpZgbopPL630296iByyigQCPJVzdusFrQN5DeC-zT_nGypQkZanLb4ZspSx9Q',
          },
          format: 'jwk',
        });
      },
      {
        message: 'RSA JWK missing e parameter',
      }
    );
  },
};

export const ec_public_key_jwk_import = {
  test() {
    const jwk = {
      crv: 'P-256',
      kty: 'EC',
      x: 'X0mMYR_uleZSIPjNztIkAS3_ud5LhNpbiIFp6fNf2Gs',
      y: 'UbJuPy2Xi0lW7UYTBxPK3yGgDu9EAKYIecjkHX5s2lI',
    };

    const key = createPublicKey({ key: jwk, format: 'jwk' });
    strictEqual(key.asymmetricKeyType, 'ec');
    deepStrictEqual(key.asymmetricKeyDetails, { namedCurve: 'prime256v1' });

    // The fail due to missing information in the JWK
    throws(
      () => {
        createPublicKey({
          key: {
            kty: 'EC',
          },
          format: 'jwk',
        });
      },
      {
        message: 'EC JWK missing crv parameter',
      }
    );

    throws(
      () => {
        createPublicKey({
          key: {
            kty: 'EC',
            crv: 'P-256',
          },
          format: 'jwk',
        });
      },
      {
        message: 'EC JWK missing x parameter',
      }
    );

    throws(
      () => {
        createPublicKey({
          key: {
            kty: 'EC',
            crv: 'P-256',
            x: 'X0mMYR_uleZSIPjNztIkAS3_ud5LhNpbiIFp6fNf2Gs',
          },
          format: 'jwk',
        });
      },
      {
        message: 'EC JWK missing y parameter',
      }
    );
  },
};

export const ed_public_key_jwk_import = {
  test() {
    const jwk = {
      crv: 'Ed25519',
      x: 'K1wIouqnuiA04b3WrMa-xKIKIpfHetNZRv3h9fBf768',
      kty: 'OKP',
    };

    const key = createPublicKey({ key: jwk, format: 'jwk' });
    strictEqual(key.asymmetricKeyType, 'ed25519');

    const jwk2 = {
      crv: 'X25519',
      x: 'K1wIouqnuiA04b3WrMa-xKIKIpfHetNZRv3h9fBf768',
      kty: 'OKP',
    };

    const key2 = createPublicKey({ key: jwk2, format: 'jwk' });
    strictEqual(key2.asymmetricKeyType, 'x25519');

    // The fail due to missing information in the JWK
    throws(
      () => {
        createPublicKey({
          key: {
            kty: 'OKP',
          },
          format: 'jwk',
        });
      },
      {
        message: 'OKP JWK missing crv parameter',
      }
    );
  },
};

export const generate_hmac_secret_key = {
  async test() {
    // Length is intentionally not a multiple of 8
    const key = generateKeySync('hmac', { length: 33 });
    strictEqual(key.type, 'secret');
    strictEqual(key.symmetricKeySize, 4);

    const { promise, resolve, reject } = Promise.withResolvers();
    generateKey('hmac', { length: 33 }, (err, key) => {
      if (err) {
        reject(err);
        return;
      }
      strictEqual(key.type, 'secret');
      strictEqual(key.symmetricKeySize, 4);
      resolve();
    });
    await promise;

    throws(() => generateKeySync('hmac', { length: 0 }), {
      message:
        'The value of "options.length" is out of range. It must ' +
        'be >= 8 && <= 65536. Received 0',
    });
    throws(() => generateKeySync('hmac', { length: 65537 }), {
      message:
        'The value of "options.length" is out of range. It must ' +
        'be >= 8 && <= 65536. Received 65537',
    });

    const h = createHmac('sha256', key);
    ok(h.update('test').digest());
  },
};

export const generate_aes_secret_key = {
  async test() {
    const key = generateKeySync('aes', { length: 128 });
    strictEqual(key.type, 'secret');
    strictEqual(key.symmetricKeySize, 16);

    const { promise, resolve, reject } = Promise.withResolvers();
    generateKey('aes', { length: 128 }, (err, key) => {
      if (err) {
        reject(err);
        return;
      }
      strictEqual(key.type, 'secret');
      strictEqual(key.symmetricKeySize, 16);
      resolve();
    });
    await promise;

    throws(() => generateKeySync('aes', { length: 0 }), {
      message:
        "The property 'options.length' must be one of: 128, 192, " +
        '256. Received 0',
    });
  },
};

export const generate_key_pair_arg_validation = {
  async test() {
    throws(() => generateKeyPairSync('invalid type'), {
      message:
        "The argument 'type' must be one of: 'rsa', " +
        "'ec', 'ed25519', 'x25519', 'dh'. Received " +
        "'invalid type'",
    });

    throws(() => generateKeyPairSync('rsa', { modulusLength: -1 }), {
      message:
        'The value of "options.modulusLength" is out of range. ' +
        'It must be >= 0 && < 4294967296. Received -1',
    });

    throws(() => generateKeyPairSync('rsa', { modulusLength: 'foo' }), {
      message:
        'The "options.modulusLength" property must be of type number. ' +
        "Received type string ('foo')",
    });

    throws(
      () =>
        generateKeyPairSync('rsa', {
          modulusLength: 512,
          publicExponent: 'foo',
        }),
      {
        message:
          'The "options.publicExponent" property must be of type number. ' +
          "Received type string ('foo')",
      }
    );

    // TODO(later): BoringSSL does not currently support rsa-pss key generation
    // in the way Node.js does. Uncomment these later when it does.
    // throws(
    //   () =>
    //     generateKeyPairSync('rsa-pss', {
    //       modulusLength: 512,
    //       hashAlgorithm: false,
    //     }),
    //   {
    //     message:
    //       'The "options.hashAlgorithm" property must be of type string. ' +
    //       'Received type boolean (false)',
    //   }
    // );

    // throws(
    //   () =>
    //     generateKeyPairSync('rsa-pss', {
    //       modulusLength: 512,
    //       mgf1HashAlgorithm: false,
    //     }),
    //   {
    //     message:
    //       'The "options.mgf1HashAlgorithm" property must be of type ' +
    //       'string. Received type boolean (false)',
    //   }
    // );

    // throws(
    //   () =>
    //     generateKeyPairSync('rsa-pss', {
    //       modulusLength: 512,
    //       saltLength: 'foo',
    //     }),
    //   {
    //     message:
    //       'The "options.saltLength" property must be of type number. ' +
    //       "Received type string ('foo')",
    //   }
    // );

    // TODO(later): BoringSSL currently does not support dsa key generation
    // in the same way Node.js does. Uncomment this when it does.
    // throws(
    //   () =>
    //     generateKeyPairSync('dsa', {
    //       modulusLength: 512,
    //       divisorLength: 'foo',
    //     }),
    //   {
    //     message:
    //       'The "options.divisorLength" property must be of type number. ' +
    //       "Received type string ('foo')",
    //   }
    // );

    throws(() => generateKeyPairSync('dh', { prime: 'foo' }), {
      message:
        'The "options.prime" property must be an instance of Buffer, ' +
        "TypedArray, or ArrayBuffer. Received type string ('foo')",
    });

    throws(() => generateKeyPairSync('dh', { primeLength: 'foo' }), {
      message:
        'The "options.primeLength" property must be of type number. ' +
        "Received type string ('foo')",
    });

    throws(() => generateKeyPairSync('dh', { primeLength: -1 }), {
      message:
        'The value of "options.primeLength" is out of range. It ' +
        'must be >= 0 && <= 2147483647. Received -1',
    });

    throws(
      () => generateKeyPairSync('dh', { primeLength: 10, generator: -1 }),
      {
        message:
          'The value of "options.generator" is out of range. It must ' +
          'be >= 0 && <= 2147483647. Received -1',
      }
    );

    throws(() => generateKeyPairSync('dh', { groupName: 123 }), {
      message:
        'The "options.group" property must be of type string. ' +
        'Received type number (123)',
    });

    throws(
      () =>
        generateKeyPairSync('ec', { namedCurve: 'foo', paramEncoding: 'foo' }),
      {
        message:
          "The property 'options.paramEncoding' must be one of: " +
          "'named', 'explicit'. Received 'foo'",
      }
    );

    throws(() => generateKeyPairSync('ed25519', { publicKeyEncoding: 'foo' }), {
      message:
        'The "options.publicKeyEncoding" property must be of type ' +
        "object. Received type string ('foo')",
    });

    throws(() => generateKeyPairSync('x25519', { privateKeyEncoding: 'foo' }), {
      message:
        'The "options.privateKeyEncoding" property must be of type ' +
        "object. Received type string ('foo')",
    });

    // ====
    async function wrapped(...args) {
      const { promise, resolve, reject } = Promise.withResolvers();
      generateKeyPair(...args, (err) => {
        if (err) {
          reject(err);
          return;
        }
        resolve();
      });
      await promise;
    }

    await rejects(wrapped('invalid type', {}), {
      message:
        "The argument 'type' must be one of: 'rsa', " +
        "'ec', 'ed25519', 'x25519', 'dh'. Received " +
        "'invalid type'",
    });

    await rejects(wrapped('rsa', { modulusLength: -1 }), {
      message:
        'The value of "options.modulusLength" is out of range. ' +
        'It must be >= 0 && < 4294967296. Received -1',
    });

    await rejects(wrapped('rsa', { modulusLength: 'foo' }), {
      message:
        'The "options.modulusLength" property must be of type number. ' +
        "Received type string ('foo')",
    });

    await rejects(
      wrapped('rsa', { modulusLength: 512, publicExponent: 'foo' }),
      {
        message:
          'The "options.publicExponent" property must be of type number. ' +
          "Received type string ('foo')",
      }
    );

    // TODO(later): BoringSSL currently does not support rsa-pss key generation
    // in the same way Node.js does. Uncomment these later when it does.
    // await rejects(
    //   wrapped('rsa-pss', { modulusLength: 512, hashAlgorithm: false }),
    //   {
    //     message:
    //       'The "options.hashAlgorithm" property must be of type string. ' +
    //       'Received type boolean (false)',
    //   }
    // );

    // await rejects(
    //   wrapped('rsa-pss', { modulusLength: 512, mgf1HashAlgorithm: false }),
    //   {
    //     message:
    //       'The "options.mgf1HashAlgorithm" property must be of type ' +
    //       'string. Received type boolean (false)',
    //   }
    // );

    // await rejects(
    //   wrapped('rsa-pss', { modulusLength: 512, saltLength: 'foo' }),
    //   {
    //     message:
    //       'The "options.saltLength" property must be of type number. ' +
    //       "Received type string ('foo')",
    //   }
    // );

    // TODO(later): BoringSSL currently does not support dsa key generation
    // in the same way Node.js does. Uncomment this later when it does.
    // await rejects(
    //   wrapped('dsa', { modulusLength: 512, divisorLength: 'foo' }),
    //   {
    //     message:
    //       'The "options.divisorLength" property must be of type number. ' +
    //       "Received type string ('foo')",
    //   }
    // );

    await rejects(wrapped('dh', { prime: 'foo' }), {
      message:
        'The "options.prime" property must be an instance of Buffer, ' +
        "TypedArray, or ArrayBuffer. Received type string ('foo')",
    });

    await rejects(wrapped('dh', { primeLength: 'foo' }), {
      message:
        'The "options.primeLength" property must be of type number. ' +
        "Received type string ('foo')",
    });

    await rejects(wrapped('dh', { primeLength: -1 }), {
      message:
        'The value of "options.primeLength" is out of range. It ' +
        'must be >= 0 && <= 2147483647. Received -1',
    });

    await rejects(wrapped('dh', { primeLength: 10, generator: -1 }), {
      message:
        'The value of "options.generator" is out of range. It must ' +
        'be >= 0 && <= 2147483647. Received -1',
    });

    await rejects(wrapped('dh', { groupName: 123 }), {
      message:
        'The "options.group" property must be of type string. ' +
        'Received type number (123)',
    });

    await rejects(wrapped('ec', { namedCurve: 'foo', paramEncoding: 'foo' }), {
      message:
        "The property 'options.paramEncoding' must be one of: " +
        "'named', 'explicit'. Received 'foo'",
    });

    await rejects(wrapped('ed25519', { publicKeyEncoding: 'foo' }), {
      message:
        'The "options.publicKeyEncoding" property must be of type ' +
        "object. Received type string ('foo')",
    });

    await rejects(wrapped('x25519', { privateKeyEncoding: 'foo' }), {
      message:
        'The "options.privateKeyEncoding" property must be of type ' +
        "object. Received type string ('foo')",
    });
  },
};

export const generate_rsa_key_pair = {
  test() {
    const { publicKey, privateKey } = generateKeyPairSync('rsa', {
      modulusLength: 2048,
    });
    strictEqual(publicKey.type, 'public');
    strictEqual(publicKey.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(publicKey.asymmetricKeyDetails.publicExponent, 65537n);

    strictEqual(privateKey.type, 'private');
    strictEqual(privateKey.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(privateKey.asymmetricKeyDetails.publicExponent, 65537n);
  },
};

export const generate_rsa_key_pair_enc = {
  test() {
    const { publicKey, privateKey } = generateKeyPairSync('rsa', {
      modulusLength: 2048,
      publicKeyEncoding: {
        format: 'der',
        type: 'pkcs1',
      },
      privateKeyEncoding: {
        format: 'pem',
        type: 'pkcs8',
      },
    });

    const pubImported = createPublicKey({
      key: publicKey,
      format: 'der',
      type: 'pkcs1',
    });
    strictEqual(pubImported.type, 'public');
    strictEqual(pubImported.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(pubImported.asymmetricKeyDetails.publicExponent, 65537n);

    const imported = createPrivateKey(privateKey);
    strictEqual(imported.type, 'private');
    strictEqual(imported.asymmetricKeyDetails.modulusLength, 2048);
    strictEqual(imported.asymmetricKeyDetails.publicExponent, 65537n);
  },
};

export const generate_ec_key_pair = {
  test() {
    const { privateKey, publicKey } = generateKeyPairSync('ec', {
      namedCurve: 'P-256',
    });
    strictEqual(publicKey.type, 'public');
    strictEqual(publicKey.asymmetricKeyDetails.namedCurve, 'prime256v1');
    strictEqual(privateKey.type, 'private');
    strictEqual(privateKey.asymmetricKeyDetails.namedCurve, 'prime256v1');
  },
};

export const generate_ed25519_key_pair = {
  test() {
    const { privateKey, publicKey } = generateKeyPairSync('ed25519', {});
    strictEqual(publicKey.type, 'public');
    strictEqual(privateKey.type, 'private');
    strictEqual(publicKey.asymmetricKeyType, 'ed25519');
    strictEqual(privateKey.asymmetricKeyType, 'ed25519');
  },
};

export const generate_x25519_key_pair = {
  test() {
    const { privateKey, publicKey } = generateKeyPairSync('x25519', {});
    strictEqual(publicKey.type, 'public');
    strictEqual(privateKey.type, 'private');
    strictEqual(publicKey.asymmetricKeyType, 'x25519');
    strictEqual(privateKey.asymmetricKeyType, 'x25519');
  },
};

// TODO(later): BoringSSL with fips does not support generating DH keys
// this way. Uncomment this later when it does.
// export const generate_dh_key_pair = {
//   test() {
//     const { privateKey, publicKey } = generateKeyPairSync('dh', {
//       group: 'modp14',
//     });
//     strictEqual(publicKey.type, 'public');
//     strictEqual(privateKey.type, 'private');
//     strictEqual(publicKey.asymmetricKeyType, 'dh');
//     strictEqual(privateKey.asymmetricKeyType, 'dh');

//     const res = diffieHellman({ privateKey, publicKey });
//     ok(res instanceof Buffer);
//     strictEqual(res.byteLength, 256);
//   },
// };

// TODO(later): BoringSSL with fips does not support generating DH keys
// this way. Uncomment this later when it does.
// export const generate_dh_from_fixed_prime = {
//   test() {
//     const prime = generatePrimeSync(1024);

//     const { privateKey: privateKey1, publicKey: publicKey1 } =
//       generateKeyPairSync('dh', {
//         prime,
//       });
//     strictEqual(publicKey1.type, 'public');
//     strictEqual(privateKey1.type, 'private');
//     strictEqual(publicKey1.asymmetricKeyType, 'dh');
//     strictEqual(privateKey1.asymmetricKeyType, 'dh');

//     const { privateKey: privateKey2, publicKey: publicKey2 } =
//       generateKeyPairSync('dh', {
//         prime,
//       });
//     strictEqual(publicKey2.type, 'public');
//     strictEqual(privateKey2.type, 'private');
//     strictEqual(publicKey2.asymmetricKeyType, 'dh');
//     strictEqual(privateKey2.asymmetricKeyType, 'dh');

//     ok(!publicKey1.equals(publicKey2));
//     ok(!privateKey1.equals(privateKey2));

//     // Once we generate the keys, let's make sure they are usable.

//     const res1 = diffieHellman({
//       privateKey: privateKey2,
//       publicKey: publicKey1,
//     });
//     ok(res1 instanceof Buffer);
//     strictEqual(res1.byteLength, 128);

//     const res2 = diffieHellman({
//       privateKey: privateKey2,
//       publicKey: publicKey1,
//     });
//     ok(res2 instanceof Buffer);
//     strictEqual(res2.byteLength, 128);

//     deepStrictEqual(res1, res2);
//     // It's actual data and not just zeroes right?
//     notDeepStrictEqual(res1, Buffer.alloc(128, 0));

//     // Keys generated from different prime groups aren't compatible and should throw.
//     const prime2 = generatePrimeSync(1024);
//     const { privateKey: privateKey3, publicKey: publicKey3 } =
//       generateKeyPairSync('dh', {
//         prime: prime2,
//       });
//     strictEqual(publicKey3.type, 'public');
//     strictEqual(privateKey3.type, 'private');
//     strictEqual(publicKey3.asymmetricKeyType, 'dh');
//     strictEqual(privateKey3.asymmetricKeyType, 'dh');

//     throws(
//       () => diffieHellman({ publicKey: publicKey1, privateKey: privateKey3 }),
//       {
//         message: 'Failed to derive shared diffie-hellman secret',
//       }
//     );
//   },
// };

export const generate_dh_key_pair_by_length = {
  test() {
    throws(
      () =>
        generateKeyPairSync('dh', {
          primeLength: 1048,
        }),
      {
        message:
          'Generating DH keys from a prime length is not yet implemented',
      }
    );
  },
};

export const generate_ed_keypair_promisified = {
  async test() {
    const promisifiedGenKeyPair = promisify(generateKeyPair);
    const { publicKey, privateKey } = await promisifiedGenKeyPair(
      'ed25519',
      {}
    );
    strictEqual(publicKey.asymmetricKeyType, 'ed25519');
    strictEqual(privateKey.asymmetricKeyType, 'ed25519');
  },
};
