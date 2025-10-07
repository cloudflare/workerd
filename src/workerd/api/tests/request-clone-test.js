import { deepStrictEqual } from 'node:assert';

// This test validates tee'ing value-oriented js-backed readable streams
// as there is no problem with byte-oriented systems.
export const clone = {
  async test() {
    function createReadableStreamFromString(str) {
      const encoder = new TextEncoder();
      const encodedData = encoder.encode(str);

      return new ReadableStream({
        start(controller) {
          // Enqueue the data and close the stream
          controller.enqueue(encodedData);
          controller.close();
        },
      });
    }

    const cr = new Request(new URL('http://localhost/test'), {
      method: 'POST',
      body: createReadableStreamFromString('Hello stream'),
      duplex: 'half',
    });

    const cr2 = cr.clone();

    deepStrictEqual(await cr.text(), await cr2.text());
  },
};
