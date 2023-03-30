import {
  randomFill,
  randomFillSync,
  randomBytes,
  randomInt,
  randomUUID,
  generateKey,
  generateKeySync,
  generateKeyPair,
  generateKeyPairSync,
  createPrivateKey,
  createSecretKey,
  PrivateKeyObject,
  PublicKeyObject,
  SecretKeyObject,
} from 'node:crypto';

import { promisify } from 'node:util';
import { Buffer } from 'node:buffer';

const kPrivateKeyPem = `
-----BEGIN ENCRYPTED PRIVATE KEY-----
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
-----END ENCRYPTED PRIVATE KEY-----
`;

export default {
  async fetch() {

    // Random bytes, numbers, and UUIDs
    const buf = new Uint8Array(10);
    randomFillSync(buf);
    console.log(buf);
    await promisify(randomFill)(buf);
    console.log(buf);
    console.log(randomBytes(10));
    console.log(await promisify(randomBytes)(10));
    console.log(randomInt(0, 10));
    console.log(randomInt(10));
    console.log(await promisify(randomInt)(10));
    console.log(randomUUID());

    // Keys
    {
      // Importing a private key from PEM...
      const pkey = createPrivateKey({
        key: kPrivateKeyPem,
        passphrase: 'top secret',
      });
      console.log(pkey.type, pkey instanceof PrivateKeyObject);
    }

    {
      // Importing a secret key from a buffer...
      const skey = createSecretKey(Buffer.from('hello'));
      console.log(skey.type, skey instanceof SecretKeyObject);
    }

    {
      // Generating a random 128-bit AES key...
      const skey = generateKeySync("aes", { length: 128 });
      console.log(skey.type, skey instanceof SecretKeyObject);
    }

    {
      // Generating an RSA key pair...
      const {
        publicKey,
        privateKey,
      } = generateKeyPairSync("rsa", {});
      console.log(publicKey.type, publicKey instanceof PublicKeyObject);
      console.log(privateKey.type, privateKey instanceof PrivateKeyObject);
      console.log(publicKey.asymmetricKeyType, publicKey.asymmetricKeyDetails);
    }

    {
      // Generating ED25519 key pair.
      const {
        publicKey,
        privateKey,
      } = generateKeyPairSync("ed25519", {});
      console.log(publicKey.type, publicKey instanceof PublicKeyObject);
      console.log(privateKey.type, privateKey instanceof PrivateKeyObject);
      console.log(publicKey.asymmetricKeyType, publicKey.asymmetricKeyDetails);
    }

    {
      // Generating x25519 key pair.
      const {
        publicKey,
        privateKey,
      } = generateKeyPairSync("x25519", {});
      console.log(publicKey.type, publicKey instanceof PublicKeyObject);
      console.log(privateKey.type, privateKey instanceof PrivateKeyObject);
      console.log(publicKey.asymmetricKeyType, publicKey.asymmetricKeyDetails);
    }

    {
      // Generating EC key pair.
      const {
        publicKey,
        privateKey,
      } = generateKeyPairSync("ec", {
        // TODO(now): Use Node.js names
        namedCurve: "P-256"
      });
      console.log(publicKey.type, publicKey instanceof PublicKeyObject);
      console.log(privateKey.type, privateKey instanceof PrivateKeyObject);
    }

    return new Response("ok");
  }
};
