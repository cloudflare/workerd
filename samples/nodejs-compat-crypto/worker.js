import {
  randomFill,
  randomFillSync,
  randomBytes,
  randomInt,
  randomUUID,
  scryptSync,
} from 'node:crypto';

import { promisify } from 'node:util';

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

    // Scrypt
    const password = 'password';
    const salt = 'salt';
    const keylen = 64;
    const options = {
      N: 16384,
      r: 8,
      p: 1,
      maxmem: 32 << 20,
    };
    console.log(scryptSync(password, salt, keylen, options));

    return new Response("ok");
  }
};
