// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests specific to the TypeScript FileSystemWritableFileStream. Behavior shared
// with the C++ implementation is covered by webfs-test.js, which both
// implementations run unmodified; this file covers what only the TypeScript
// version can do, plus the edge cases neither had coverage for.

import { strictEqual, ok, rejects, throws } from 'node:assert';

// The storage root is read-only; `tmp` is the writable directory in it.
async function newFile(name) {
  const dir = await navigator.storage.getDirectory();
  const tmp = await dir.getDirectoryHandle('tmp');
  return await tmp.getFileHandle(name, { create: true });
}

async function contentsOf(handle) {
  return await (await handle.getFile()).text();
}

// The reason this implementation exists: with typescript_implemented_streams
// enabled, the C++ FileSystemWritableFileStream inherits from the C++
// WritableStream, which is no longer the WritableStream user code sees. These
// assertions fail against the C++ implementation, so they double as proof that
// the swap took effect — without them the rest of this file would pass either
// way.
export const isRealWritableStreamSubclass = {
  async test() {
    const file = await newFile('subclass.txt');
    const writable = await file.createWritable();
    ok(
      writable instanceof WritableStream,
      'FileSystemWritableFileStream instance must be a WritableStream'
    );
    strictEqual(
      Object.getPrototypeOf(FileSystemWritableFileStream),
      WritableStream,
      'FileSystemWritableFileStream must extend the global WritableStream'
    );
    strictEqual(
      Object.getPrototypeOf(FileSystemWritableFileStream.prototype),
      WritableStream.prototype
    );
    // Inherited members must be reachable, not shadowed.
    strictEqual(typeof writable.getWriter, 'function');
    strictEqual(writable.locked, false);
    await writable.abort();
  },
};

// The user-visible consequence of the above: piping into the stream. pipeTo
// checks its destination with a brand check, so a foreign WritableStream is
// rejected outright.
export const pipeToWorks = {
  async test() {
    const file = await newFile('pipe.txt');
    const writable = await file.createWritable();
    const readable = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('piped '));
        c.enqueue(new TextEncoder().encode('contents'));
        c.close();
      },
    });
    // pipeTo closes the destination on completion, which commits the file.
    await readable.pipeTo(writable);
    strictEqual(await contentsOf(file), 'piped contents');
  },
};

// createWritable() is installed over a JSG method, so the descriptor shape has
// to match what JSG_METHOD produced or the swap is observable.
export const createWritableDescriptorMatchesJsg = {
  test() {
    const desc = Object.getOwnPropertyDescriptor(
      FileSystemFileHandle.prototype,
      'createWritable'
    );
    strictEqual(typeof desc.value, 'function');
    strictEqual(desc.enumerable, true);
    strictEqual(desc.configurable, true);
    strictEqual(desc.writable, true);
  },
};

// There is no user-reachable constructor, matching `constructor() = delete` on
// the C++ class.
export const constructorIsNotCallable = {
  test() {
    throws(() => new FileSystemWritableFileStream(), TypeError);
    // Passing something in the marker position must not get past the gate.
    throws(() => new FileSystemWritableFileStream({}, {}), TypeError);
  },
};

// JSG unwraps arguments before the method body runs, so a bad option bag is a
// synchronous throw while an unopenable file is a rejection. The TypeScript
// createWritable() is deliberately not an async function so it can keep those
// two shapes distinct.
export const badOptionsThrowSynchronously = {
  async test() {
    const file = await newFile('badopts.txt');
    throws(() => file.createWritable(5), TypeError);
    throws(() => file.createWritable('nope'), TypeError);
    // Objects, arrays and functions are all objects to JSG, so they are
    // accepted and simply have no keepExistingData field.
    for (const options of [{}, [], () => {}, null, undefined]) {
      const writable = await file.createWritable(options);
      ok(writable instanceof FileSystemWritableFileStream);
      await writable.abort();
    }
  },
};

// keepExistingData is read with ToBoolean, not type-checked, matching how JSG
// unwraps jsg::Optional<bool>. So a non-empty string opts in.
export const keepExistingDataIsCoerced = {
  async test() {
    const file = await newFile('coerce.txt');
    const seed = await file.createWritable();
    await seed.write('0123456789');
    await seed.close();

    const writable = await file.createWritable({ keepExistingData: 'false' });
    await writable.write('ab');
    await writable.close();
    // Opted in, so only the first two bytes were replaced.
    strictEqual(await contentsOf(file), 'ab23456789');
  },
};

// A foreign receiver must be rejected the way JSG would have rejected it.
export const createWritableRejectsForeignReceiver = {
  async test() {
    const { createWritable } = FileSystemFileHandle.prototype;
    throws(() => createWritable.call({}), TypeError);
    throws(() => createWritable.call(null), TypeError);
    // A directory handle is a FileSystemHandle but not a file handle.
    const dir = await navigator.storage.getDirectory();
    throws(() => createWritable.call(dir), TypeError);
  },
};

// The stream methods are brand-checked, so they cannot be applied to a foreign
// object even though they live on a user-reachable prototype.
export const methodsAreBrandChecked = {
  async test() {
    const file = await newFile('brand.txt');
    const writable = await file.createWritable();
    const { write, seek, truncate } = FileSystemWritableFileStream.prototype;
    throws(() => write.call({}, 'x'), TypeError);
    throws(() => seek.call({}, 0), TypeError);
    throws(() => truncate.call({}, 0), TypeError);
    // A plain WritableStream has no context either.
    const plain = new WritableStream();
    throws(() => write.call(plain, 'x'), TypeError);
    await writable.abort();
  },
};

// write() acquires a writer as a mutual-exclusion check, so it fails on a
// stream that is already locked. seek() and truncate() deliberately do not, so
// they still work. This preserves the C++ behavior, including the message,
// which says "reader" despite the stream being writable.
export const writeRejectsWhenLockedButSeekDoesNot = {
  async test() {
    const file = await newFile('locked.txt');
    const writable = await file.createWritable();
    const writer = writable.getWriter();
    strictEqual(writable.locked, true);

    throws(() => writable.write('x'), {
      name: 'TypeError',
      message: 'Cannot write to a stream that is locked to a reader',
    });
    // Not gated on the lock.
    await writable.seek(0);
    await writable.truncate(0);

    writer.releaseLock();
    await writable.write('now unlocked');
    await writable.close();
    strictEqual(await contentsOf(file), 'now unlocked');
  },
};

// Writing through the writer goes through the sink rather than the direct
// method, and must land in the same context.
export const writerWritesReachTheSameFile = {
  async test() {
    const file = await newFile('viawriter.txt');
    const writable = await file.createWritable();
    const writer = writable.getWriter();
    await writer.write(new TextEncoder().encode('through '));
    await writer.write(new TextEncoder().encode('the writer'));
    await writer.close();
    strictEqual(await contentsOf(file), 'through the writer');
  },
};

// abort() runs the sink's abort algorithm, which discards the temporary and
// leaves the original intact.
export const abortDiscardsWrites = {
  async test() {
    const file = await newFile('abort.txt');
    const seed = await file.createWritable();
    await seed.write('original');
    await seed.close();

    const writable = await file.createWritable({ keepExistingData: true });
    await writable.write('clobbered');
    await writable.abort();
    strictEqual(await contentsOf(file), 'original');
  },
};

// Once the context is finished, further operations fail rather than reaching a
// released temporary.
export const operationsAfterCloseFail = {
  async test() {
    const file = await newFile('afterclose.txt');
    const writable = await file.createWritable();
    await writable.write('done');
    await writable.close();

    await rejects(writable.seek(0), TypeError);
    await rejects(writable.truncate(0), TypeError);
    strictEqual(await contentsOf(file), 'done');
  },
};

// The tag is what Object.prototype.toString reports, and is part of the
// user-visible surface JSG installs.
export const hasCorrectStringTag = {
  async test() {
    const file = await newFile('tag.txt');
    const writable = await file.createWritable();
    strictEqual(
      Object.prototype.toString.call(writable),
      '[object FileSystemWritableFileStream]'
    );
    await writable.abort();
  },
};
