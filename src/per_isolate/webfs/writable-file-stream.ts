'use strict';

// FileSystemWritableFileStream — the File System Access API's writable file
// handle.
//
// The file semantics are done by a native context obtained from
// utils.createFileSystemWriteContext(); this module owns only the stream
// wiring. See FileSystemWriteContextHandle in src/workerd/api/filesystem.h.
//
// CONSTRUCTION
//
// Nothing constructs this from JavaScript: the class has no user-reachable
// constructor and instances come only from FileSystemFileHandle.createWritable().
// That method is a C++ JSG method, so the TypeScript version is installed over
// it on the prototype by main.ts, and createWritable() below is what runs. The
// C++ createWritable() stays in place and is what runs when the flag is off.
//
// WRITES BYPASS THE QUEUE
//
// write(), seek() and truncate() do not enqueue anything. They reach the native
// context directly, exactly as the C++ methods do, so a write is complete when
// its promise settles rather than when the queue drains. Chunks written through
// a writer (or by pipeTo) go through the sink instead, which lands in the same
// context.
//
// write() additionally acquires and immediately releases a writer. Acquiring
// is what makes a stream already locked to a writer fail, and it is the C++
// behavior being preserved. seek() and truncate() deliberately do not do this,
// so they succeed on a locked stream.
//
// TRANSACTIONAL
//
// Writes land in a temporary file and the VFS lock is held from createWritable()
// until close() or abort(). close() replaces the original file's contents;
// abort() discards them. A stream that is never closed nor aborted leaves the
// lock held until the context is collected.

import type {
  UnderlyingSink,
  WritableStream as WritableStreamType,
  WritableStreamDefaultWriter as WritableStreamDefaultWriterType,
} from '../webstreams/types';

const {
  DOMException: NativeDOMException,
  ObjectDefineProperties,
  PromiseReject,
  PromiseResolve,
  SymbolToStringTag,
  TypeError,
} = primordials;

const { createFileSystemWriteContext } = utils;

const {
  WritableStream,
  internalsForPipe: { isWritableStreamLocked, acquireWriter, writerRelease },
} = require('webstreams/writable') as {
  WritableStream: typeof WritableStreamType;
  internalsForPipe: {
    isWritableStreamLocked(stream: unknown): boolean;
    acquireWriter(stream: unknown): WritableStreamDefaultWriterType<unknown>;
    writerRelease(writer: WritableStreamDefaultWriterType<unknown>): void;
  };
};

// Gates the constructor. createWritable() is the only holder, so `new
// FileSystemWritableFileStream()` from user code cannot produce an instance —
// matching `constructor() = delete` on the C++ class.
const kConstruct = {};

function isActualObject(value: unknown): boolean {
  return value != null && typeof value === 'object';
}

class FileSystemWritableFileStream extends WritableStream<unknown> {
  #context: FileSystemWriteContext;

  constructor(marker?: unknown, context?: FileSystemWriteContext) {
    if (marker !== kConstruct) {
      throw new TypeError('Illegal constructor');
    }
    // The sink closures capture the context rather than `this`, which is in its
    // temporal dead zone until super() returns.
    const ctx = context as FileSystemWriteContext;
    const sink: UnderlyingSink<unknown> = {
      write(chunk: unknown): Promise<void> {
        return ctx.write(chunk as ArrayBuffer);
      },
      close(): Promise<void> {
        return ctx.commit();
      },
      abort(): void {
        ctx.discard();
      },
    };
    super(sink);
    this.#context = ctx;
  }

  #getContext(): FileSystemWriteContext {
    if (!isActualObject(this) || !(#context in this)) {
      throw new TypeError('Illegal invocation');
    }
    return this.#context;
  }

  write(data: unknown): Promise<void> {
    const context = this.#getContext();
    // Acquiring the writer is what rejects a stream that is already locked. The
    // message says "reader" because that is what C++ says; it is preserved so
    // both implementations report the same thing.
    if (isWritableStreamLocked(this)) {
      throw new TypeError(
        'Cannot write to a stream that is locked to a reader'
      );
    }
    const writer = acquireWriter(this);
    try {
      return context.write(data as ArrayBuffer);
    } finally {
      // Released before the returned promise settles, as the C++ KJ_DEFER does.
      writerRelease(writer);
    }
  }

  seek(position: unknown): Promise<void> {
    return this.#getContext().seek(position as number);
  }

  truncate(size: unknown): Promise<void> {
    return this.#getContext().truncate(size as number);
  }
}

// The tag is set unconditionally, as it is throughout the TypeScript streams
// implementation. JSG instead gates it on the set_tostring_tag compat flag, so a
// worker with a compatibility date before that flag's 2024-09-26 default sees a
// tag here where the C++ implementation exposes none.
ObjectDefineProperties(FileSystemWritableFileStream.prototype, {
  __proto__: null,
  write: { __proto__: null, enumerable: true },
  seek: { __proto__: null, enumerable: true },
  truncate: { __proto__: null, enumerable: true },
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'FileSystemWritableFileStream',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

// Mirrors how JSG unwraps `jsg::Optional<FileSystemCreateWritableOptions>`:
// undefined and null give an empty bag, any object has its field read with
// ToBoolean, and a primitive is a TypeError. Arrays and functions are objects
// here, as they are to v8::Value::IsObject(), so they are accepted and simply
// have no field. The message reproduces the one JSG generates.
function readKeepExistingData(options: unknown): boolean {
  if (options === undefined || options === null) return false;
  if (typeof options !== 'object' && typeof options !== 'function') {
    throw new TypeError(
      "Failed to execute 'createWritable' on 'FileSystemFileHandle': " +
        "parameter 1 is not of type 'FileSystemCreateWritableOptions'."
    );
  }
  // A user-provided bag, so ordinary property access and ToBoolean are correct.
  return !!(options as { keepExistingData?: unknown }).keepExistingData;
}

// Installed over FileSystemFileHandle.prototype.createWritable by main.ts.
//
// Deliberately not an async function. JSG unwraps arguments before the method
// body runs, so a bad option bag is a synchronous throw in C++, while a file
// that cannot be opened is a rejection. An async function would turn the former
// into a rejection too, so the two failures are shaped separately here.
//
// The option bag is read before the receiver is checked, the opposite of JSG's
// order. A call that is wrong in both ways reports the bag where C++ reports the
// receiver; both are synchronous TypeErrors, so only the message differs.
function createWritable(
  this: unknown,
  options?: unknown
): Promise<FileSystemWritableFileStream> {
  const keepExistingData = readKeepExistingData(options);
  try {
    const context = createFileSystemWriteContext(
      this as object,
      keepExistingData
    );
    return PromiseResolve(
      new FileSystemWritableFileStream(kConstruct, context)
    );
  } catch (err) {
    // A DOMException means the file could not be opened, which createWritable
    // reports by rejecting. Anything else is the receiver check failing, which
    // JSG performs before the method body runs and so throws.
    if (err instanceof NativeDOMException) {
      return PromiseReject(err);
    }
    throw err;
  }
}

module.exports = {
  FileSystemWritableFileStream,
  createWritable,
};
