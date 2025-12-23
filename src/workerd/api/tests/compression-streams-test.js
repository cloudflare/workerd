import { strictEqual } from 'node:assert';

// TODO(cleanup): This is a copy of an existing test in streams-test. Once the autogate is remvoed,
// this separate test can be deleted.
export const test = {
  async test() {
    const cs = new CompressionStream('gzip');
    const cw = cs.writable.getWriter();
    await cw.write(new TextEncoder().encode('0123456789'.repeat(1000)));
    await cw.close();
    const data = await new Response(cs.readable).arrayBuffer();
    strictEqual(66, data.byteLength);

    const ds = new DecompressionStream('gzip');
    const dw = ds.writable.getWriter();
    await dw.write(data);
    await dw.close();

    const read = await new Response(ds.readable).arrayBuffer();
    strictEqual(10_000, read.byteLength);
  },
};
