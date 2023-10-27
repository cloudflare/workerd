import {
  strictEqual,
  deepStrictEqual,
  rejects,
  throws,
} from 'node:assert';

export const digeststream = {
  async test() {
    {
      const check = new Uint8Array([
        198, 247, 195, 114, 100, 29, 210,  94,  15, 221, 240,  33,  83, 117,  86, 31
      ]);

      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      const enc = new TextEncoder();

      writer.write(enc.encode('hello'));
      writer.write(enc.encode('there'));
      writer.close();

      const digest = new Uint8Array(await stream.digest);

      deepStrictEqual(digest, check);
    }

    {
      const stream = new crypto.DigestStream('md5');
      const writer = stream.getWriter();
      const enc = new TextEncoder();

      writer.write(enc.encode('hello'));
      writer.write(enc.encode('there'));
      writer.abort(new Error('boom'));

      await rejects(stream.digest);
    }

    {
      // Creating for other known types works...
      new crypto.DigestStream('SHA-256');
      new crypto.DigestStream('SHA-384');
      new crypto.DigestStream('SHA-512');

      // But fails for unknown digest names...
      throws(() => new crypto.DigestStream("foo"));
    }

    (async () => {
      let digestPromise;
      {
        digestPromise = (new crypto.DigestStream('md5')).digest;
      }
      globalThis.gc();
      await digestPromise;
      throw new Error('The promise should not have resolved');
    })();

    {
      const enc = new TextEncoder();
      const check =
        new Uint8Array([93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146]);
      const digestStream = new crypto.DigestStream('md5');
      const writer = digestStream.getWriter();
      await writer.write(enc.encode('hello'));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    // Creating and not using a digest stream doesn't crash
    new crypto.DigestStream('SHA-1');
  }
};

export const digestStreamNoEnd = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const enc = new TextEncoder();

    writer.write(enc.encode('hello'));
    writer.write(enc.encode('there'));
    // stream never ends, should not crash when IoContext is torn down.
  }
};
