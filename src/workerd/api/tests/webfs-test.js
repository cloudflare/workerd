import { ok, strictEqual, deepStrictEqual, rejects } from 'node:assert';

ok(navigator.storage instanceof StorageManager);

const dir = await navigator.storage.getDirectory();
ok(dir instanceof FileSystemDirectoryHandle);
strictEqual(dir.name, '');

const bundle = await dir.getDirectoryHandle('bundle');
const temp = await dir.getDirectoryHandle('tmp');

strictEqual(bundle.name, 'bundle');
strictEqual(temp.name, 'tmp');

let count = 0;
for (const node of dir) {
  ok(node.value instanceof FileSystemDirectoryHandle);
  count++;
}
strictEqual(count, 2);

const names = Array.from(dir.keys());
deepStrictEqual(names, ['bundle', 'tmp']);

export const webfsTest = {
  async test() {
    const file = await temp.getFileHandle('foo.txt', { create: true });
    ok(file instanceof FileSystemFileHandle);
    const writable = await file.createWritable();
    ok(writable instanceof FileSystemWritableFileStream);
    const enc = new TextEncoder();
    await writable.write(enc.encode('hello world'));
    await writable.close();
    const file2 = await temp.getFileHandle('foo.txt');
    ok(file2 instanceof FileSystemFileHandle);
    const fileStream = await file2.getFile();
    ok(fileStream instanceof File);
    const text = await fileStream.text();
    strictEqual(text, 'hello world');

    await rejects(temp.getFileHandle('does-not-exist.txt', { create: false }), {
      message: 'File not found',
    });
  },
};
