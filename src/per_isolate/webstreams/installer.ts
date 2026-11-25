'use strict';

const { ObjectDefineProperties } = primordials;

let installed = false;

function getDescriptor(value: unknown) {
  return {
    __proto__: null,
    configurable: true,
    enumerable: false,
    writable: true,
    value,
  };
}

function installWebStreams() {
  if (installed) return;

  const {
    ReadableStream,
    ReadableStreamDefaultReader,
    ReadableStreamBYOBReader,
    ReadableStreamDefaultController,
    ReadableByteStreamController,
    ReadableStreamBYOBRequest,
    ByteLengthQueuingStrategy,
    CountQueuingStrategy,
    WritableStream,
    WritableStreamDefaultWriter,
    WritableStreamDefaultController,
    TransformStream,
    TransformStreamDefaultController,
    IdentityTransformStream,
    FixedLengthStream,
    TextEncoderStream,
    TextDecoderStream,
    CompressionStream,
    DecompressionStream,
  } = require('webstreams/streams');

  ObjectDefineProperties(globalThis, {
    __proto__: null,
    ReadableStream: getDescriptor(ReadableStream),
    ReadableStreamDefaultReader: getDescriptor(ReadableStreamDefaultReader),
    ReadableStreamBYOBReader: getDescriptor(ReadableStreamBYOBReader),
    ReadableStreamDefaultController: getDescriptor(
      ReadableStreamDefaultController
    ),
    ReadableByteStreamController: getDescriptor(ReadableByteStreamController),
    ReadableStreamBYOBRequest: getDescriptor(ReadableStreamBYOBRequest),
    ByteLengthQueuingStrategy: getDescriptor(ByteLengthQueuingStrategy),
    CountQueuingStrategy: getDescriptor(CountQueuingStrategy),
    WritableStream: getDescriptor(WritableStream),
    WritableStreamDefaultWriter: getDescriptor(WritableStreamDefaultWriter),
    WritableStreamDefaultController: getDescriptor(
      WritableStreamDefaultController
    ),
    TransformStream: getDescriptor(TransformStream),
    TransformStreamDefaultController: getDescriptor(
      TransformStreamDefaultController
    ),
    IdentityTransformStream: getDescriptor(IdentityTransformStream),
    FixedLengthStream: getDescriptor(FixedLengthStream),
    TextEncoderStream: getDescriptor(TextEncoderStream),
    TextDecoderStream: getDescriptor(TextDecoderStream),
    CompressionStream: getDescriptor(CompressionStream),
    DecompressionStream: getDescriptor(DecompressionStream),
  });

  // Bootstrap the cpp exports module
  require('webstreams/cpp_exports');

  // DigestStream subclasses WritableStream, so the C++ one inherits from the
  // wrong class once the TypeScript streams are installed. The replacement has
  // the attributes JSG_NESTED_TYPE installs (all three true), so the swap is
  // invisible to property introspection, and loads on first use.
  const { Crypto } = globalThis as unknown as {
    Crypto: { prototype: object };
  };
  utils.defineLazyDataProperty(
    Crypto.prototype,
    'DigestStream',
    'crypto/digest-stream',
    true
  );

  installed = true;
}

// FileSystemWritableFileStream is the other WritableStream subclass, and has
// the same problem DigestStream does. Its instances come from
// FileSystemFileHandle.createWritable() rather than a constructor, so both the
// global and that method have to be replaced for the swap to take effect.
//
// The web file system is opt-in, so these globals only exist under
// enable_web_file_system; without it there is nothing to replace. That makes
// this depend on the flags, so only main-post.ts calls it, after
// installWebStreams().
function maybeInstallWebFileSystemStreams() {
  const { FileSystemFileHandle, FileSystemWritableFileStream: NativeStream } =
    globalThis as unknown as {
      FileSystemFileHandle?: { prototype: object };
      FileSystemWritableFileStream?: unknown;
    };
  if (FileSystemFileHandle === undefined || NativeStream === undefined) {
    return;
  }

  const {
    FileSystemWritableFileStream,
    createWritable,
  } = require('webfs/writable-file-stream');

  ObjectDefineProperties(globalThis, {
    __proto__: null,
    FileSystemWritableFileStream: getDescriptor(FileSystemWritableFileStream),
  });

  // The attributes match what JSG_METHOD installs (all three true), so the
  // swap is invisible to property introspection.
  ObjectDefineProperties(FileSystemFileHandle.prototype, {
    __proto__: null,
    createWritable: {
      __proto__: null,
      configurable: true,
      enumerable: true,
      writable: true,
      value: createWritable,
    },
  });
}

// Internal-only: expose the DrainingReader for testing expectedLength
// pass-through (Content-Length integration). Gated by a separate
// experimental flag that may never lose its experimental annotation.
function maybeInstallDrainingReader() {
  if (!compatFlags['expose_draining_reader']) return;

  const { ReadableStreamDrainingReader } = require('webstreams/streams');
  ObjectDefineProperties(globalThis, {
    __proto__: null,
    ReadableStreamDrainingReader: getDescriptor(ReadableStreamDrainingReader),
  });
}

module.exports = {
  installWebStreams,
  maybeInstallWebFileSystemStreams,
  maybeInstallDrainingReader,
};
