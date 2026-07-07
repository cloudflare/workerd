'use strict';
// Per-isolate bootstrap entry point.
// This script runs synchronously at context creation time,
// before any user code.

// This bootstrap script is called when a new context for an isolate is created.
// It is used to install any necessary global objects or functions that should
// be available at the global scope implemented in JavaScript/TypeScript. This
// runs before any user code is executed.
//
// There are several pseudo-globals available in this script:
// - `globalThis`: the global object of the context being initialized.
// - `compatFlags`: an object containing compatibility flags enabled for the context.
// - `autogates`: an object containing autogates enabled for the context.
// - `require`: a function to load other per-isolate bootstrap scripts.
// - `module`: the commonjs-style module object for this script, used for exporting values
// - `exports`: the commonjs-style exports object for this script, used for exporting values
// - `primordials`: an object containing references to built-in methods and constructors,
//   used to prevent prototype pollution
//
// The `main` script itself does not make use of the `module` or `exports`. Anything
// set on them will be ignored. They are useful only in the other scripts that are
// loaded via `require`.
//
// Circular dependencies between bootstrap scripts are not allowed. If a circular
// dependency is detected, an error will be thrown and the context will fail to
// initialize, appearing as a startup failure. If you need to share code between
// bootstrap scripts, you can create a separate module and require it from both
// scripts.

// const foo = require('./foo');
// (globalThis as any).Foo = foo.bootstrapFoo(compatFlags);
//
const { ObjectDefineProperties } = primordials;

if (compatFlags['typescript_implemented_streams']) {
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
    ReadableStreamDrainingReader,
  } = require('webstreams/streams');

  ObjectDefineProperties(globalThis, {
    ReadableStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ReadableStream,
    },
    ReadableStreamDefaultReader: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ReadableStreamDefaultReader,
    },
    ReadableStreamBYOBReader: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ReadableStreamBYOBReader,
    },
    ReadableStreamDefaultController: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ReadableStreamDefaultController,
    },
    ReadableByteStreamController: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ReadableByteStreamController,
    },
    ReadableStreamBYOBRequest: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ReadableStreamBYOBRequest,
    },
    ByteLengthQueuingStrategy: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: ByteLengthQueuingStrategy,
    },
    CountQueuingStrategy: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: CountQueuingStrategy,
    },
    WritableStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: WritableStream,
    },
    WritableStreamDefaultWriter: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: WritableStreamDefaultWriter,
    },
    WritableStreamDefaultController: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: WritableStreamDefaultController,
    },
    TransformStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: TransformStream,
    },
    TransformStreamDefaultController: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: TransformStreamDefaultController,
    },
    IdentityTransformStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: IdentityTransformStream,
    },
    FixedLengthStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: FixedLengthStream,
    },
    TextEncoderStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: TextEncoderStream,
    },
    TextDecoderStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: TextDecoderStream,
    },
  });

  // Internal-only: expose the DrainingReader for testing expectedLength
  // pass-through (Content-Length integration). Gated by a separate
  // experimental flag that may never lose its experimental annotation.
  if (compatFlags['expose_draining_reader']) {
    ObjectDefineProperties(globalThis, {
      ReadableStreamDrainingReader: {
        __proto__: null,
        configurable: true,
        enumerable: false,
        writable: true,
        value: ReadableStreamDrainingReader,
      },
    });
  }
}
