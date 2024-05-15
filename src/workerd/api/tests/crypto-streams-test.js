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

      strictEqual(stream.bytesWritten, 10n);
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

    {
      const check =
        new Uint8Array([93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146]);
      const digestStream = new crypto.DigestStream('md5');
      const writer = digestStream.getWriter();
      await writer.write('hello');
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const check = new Uint8Array([70,
        54, 153, 61, 62, 29, 164, 233, 214, 184, 248, 123, 121, 232, 247, 198, 208, 24,
        88, 13, 82, 102, 25, 80, 234, 188, 56, 69, 197, 137, 122, 77,
      ]);
      const digestStream = new crypto.DigestStream('SHA-256');
      const writer = digestStream.getWriter();
      await writer.write(new Uint32Array([1,2,3]));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const check = new Uint8Array([70,
        54, 153, 61, 62, 29, 164, 233, 214, 184, 248, 123, 121, 232, 247, 198, 208, 24,
        88, 13, 82, 102, 25, 80, 234, 188, 56, 69, 197, 137, 122, 77,
      ]);
      const digestStream = new crypto.DigestStream('SHA-256');
      const writer = digestStream.getWriter();
      // Ensures that byteOffset is correctly handled.
      await writer.write(new Uint32Array([0,1,2,3]).subarray(1));
      await writer.close();
      const digest = new Uint8Array(await digestStream.digest);
      deepStrictEqual(digest, check);
    }

    {
      const digestStream = new crypto.DigestStream('md5');
      const writer = digestStream.getWriter();

      try {
        await writer.write(123);
        throw new Error('should have failed');
      } catch (err) {
        strictEqual(err.message,
              'DigestStream is a byte stream but received an object ' +
              'of non-ArrayBuffer/ArrayBufferView/string type on its writable side.');
      }
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
    // stream never ends, should not crash.
  }
};

export const digestStreamDisposable = {
  async test() {
    const enc = new TextEncoder();
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();

    const writer = stream.getWriter();

    try {
      await writer.write(enc.encode('hello'));
      throw new Error('should have failed');
    } catch (err) {
      strictEqual(err.message, 'The DigestStream was disposed.');
    }

    // Calling dispose again should have no impact
    stream[Symbol.dispose]();
  }
};
