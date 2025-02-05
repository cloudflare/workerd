import { strictEqual, ok } from 'node:assert';
import { KeyObject, SecretKeyObject, createSecretKey } from 'node:crypto';
import { Buffer } from 'node:buffer';

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
    //    console.log(key1.toCryptoKey());

    const jwk = key3.export({ format: 'jwk' });
    ok(jwk);
    strictEqual(typeof jwk, 'object');
    strictEqual(jwk.kty, 'oct');
    strictEqual(jwk.ext, true);
    strictEqual(jwk.k, 'dGhlcmU');
  },
};
