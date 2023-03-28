import {
  randomFill,
  randomFillSync,
  randomBytes,
  randomInt,
  randomUUID,
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

    return new Response("ok");
  }
};
