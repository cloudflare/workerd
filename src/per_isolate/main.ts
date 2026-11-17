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
const { ObjectDefineProperties, ObjectDefineProperty, ObjectGetPrototypeOf } =
  primordials;

// Loaded whatever the compatibility flags, as is every module that sets up a
// cloneable class that every reader must be able to read (see
// serialization.ts).
require('serialization');

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
    CompressionStream,
    DecompressionStream,
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
    CompressionStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: CompressionStream,
    },
    DecompressionStream: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: DecompressionStream,
    },
  });

  // Bootstrap the cpp exports module
  require('webstreams/cpp_exports');

  // DigestStream subclasses WritableStream, so the C++ one would inherit from
  // the C++ WritableStream that the assignments above just replaced. Swap in
  // the TypeScript implementation so the hierarchy is consistent.
  //
  // JSG installs nested types on the prototype rather than the instance, so
  // this reaches every `crypto.DigestStream` - including the lazily created
  // `globalThis.crypto` singleton, which is deliberately not touched here.
  // The attributes match what JSG_NESTED_TYPE installs (ObjectTemplate::Set
  // with no attributes, i.e. all three true), so the swap is invisible to
  // property introspection.
  const { DigestStream } = require('crypto/digest-stream');
  const { Crypto } = globalThis as unknown as {
    Crypto: { prototype: object };
  };
  ObjectDefineProperties(Crypto.prototype, {
    __proto__: null,
    DigestStream: {
      __proto__: null,
      configurable: true,
      enumerable: true,
      writable: true,
      value: DigestStream,
    },
  });

  // FileSystemWritableFileStream is the other WritableStream subclass, and has
  // the same problem DigestStream does. Its instances come from
  // FileSystemFileHandle.createWritable() rather than a constructor, so both the
  // global and that method have to be replaced for the swap to take effect.
  //
  // The web file system is opt-in, so these globals only exist under
  // enable_web_file_system; without it there is nothing to replace.
  const { FileSystemFileHandle, FileSystemWritableFileStream: NativeStream } =
    globalThis as unknown as {
      FileSystemFileHandle?: { prototype: object };
      FileSystemWritableFileStream?: unknown;
    };
  if (FileSystemFileHandle !== undefined && NativeStream !== undefined) {
    const {
      FileSystemWritableFileStream,
      createWritable,
    } = require('webfs/writable-file-stream');

    ObjectDefineProperties(globalThis, {
      FileSystemWritableFileStream: {
        __proto__: null,
        configurable: true,
        enumerable: false,
        writable: true,
        value: FileSystemWritableFileStream,
      },
    });

    // The attributes match what JSG_METHOD installs, so the swap is invisible to
    // property introspection.
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

// The TypeScript MessageChannel and MessagePort replace the C++ ones. They are
// loaded whether or not they become globals: cloudflare-internal:messagechannel
// (used by node:worker_threads) reads them from this module's exports
// (MessageChannelModule in api/messagechannel.h).
if (autogates['ts-message-channel']) {
  const {
    MessageChannel,
    MessagePort,
  } = require('messagechannel/messagechannel');
  if (compatFlags['expose_global_message_channel']) {
    // Installed where JSG_NESTED_TYPE installs the C++ classes, with the same
    // attributes: always on the global scope's prototype, with all three true,
    // and under spec_compliant_property_attributes also as own properties of
    // the global, non-enumerable per Web IDL.
    ObjectDefineProperties(ObjectGetPrototypeOf(globalThis), {
      MessageChannel: {
        __proto__: null,
        configurable: true,
        enumerable: true,
        writable: true,
        value: MessageChannel,
      },
      MessagePort: {
        __proto__: null,
        configurable: true,
        enumerable: true,
        writable: true,
        value: MessagePort,
      },
    });
    if (compatFlags['spec_compliant_property_attributes']) {
      ObjectDefineProperties(globalThis, {
        MessageChannel: {
          __proto__: null,
          configurable: true,
          enumerable: false,
          writable: true,
          value: MessageChannel,
        },
        MessagePort: {
          __proto__: null,
          configurable: true,
          enumerable: false,
          writable: true,
          value: MessagePort,
        },
      });
    }
  }
}

// The TypeScript EventSource replaces the C++ one, which global-scope.h then
// leaves out. Loaded after the streams globals above, whose reader it
// captures.
if (autogates['ts-event-source']) {
  const { EventSource } = require('eventsource/eventsource');
  // Installed as the MessageChannel classes above are, matching
  // JSG_NESTED_TYPE.
  ObjectDefineProperties(ObjectGetPrototypeOf(globalThis), {
    EventSource: {
      __proto__: null,
      configurable: true,
      enumerable: true,
      writable: true,
      value: EventSource,
    },
  });
  if (compatFlags['spec_compliant_property_attributes']) {
    ObjectDefineProperties(globalThis, {
      EventSource: {
        __proto__: null,
        configurable: true,
        enumerable: false,
        writable: true,
        value: EventSource,
      },
    });
  }
}

// The TypeScript URL and URLSearchParams replace the C++ ones, which
// global-scope.h then leaves out. Installed as WebIDL installs an interface:
// own, non-enumerable properties of the global, and not on its prototype.
if (autogates['ts-url']) {
  const { URL, URLSearchParams } = require('url/url');
  // The C++ bridge's functions, which it finds already loaded.
  require('url/cpp_exports');
  ObjectDefineProperties(globalThis, {
    URL: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: URL,
    },
    URLSearchParams: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: URLSearchParams,
    },
  });
}

// The TypeScript URLPattern replaces the C++ ones, which global-scope.h then
// leaves out, installed as URL is.
if (autogates['ts-url-pattern']) {
  const { URLPattern } = require('urlpattern/urlpattern');
  ObjectDefineProperties(globalThis, {
    URLPattern: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: URLPattern,
    },
  });
}

// Internal-only: cloneable classes for testing the serialization protocol
// (serialization.ts), such as over RPC. Gated by an experimental flag that
// may never lose its experimental annotation.
if (compatFlags['expose_serialization_test_class']) {
  const {
    CloneableTestBox,
    CloneableTestError,
  } = require('testing/cloneable-test-box');
  ObjectDefineProperties(globalThis, {
    CloneableTestBox: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: CloneableTestBox,
    },
    CloneableTestError: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: CloneableTestError,
    },
  });
}

// Defines `name` on `target` as an accessor that loads the value on the first
// read and then becomes a data property holding it. An assignment before then
// makes the data property an assignment to it would have, with the assigned
// value.
function defineLazyGlobal(
  target: object,
  name: string,
  enumerable: boolean,
  load: () => unknown
): void {
  ObjectDefineProperty(target, name, {
    __proto__: null,
    configurable: true,
    enumerable,
    get(): unknown {
      const value = load();
      ObjectDefineProperty(target, name, {
        __proto__: null,
        configurable: true,
        enumerable,
        writable: true,
        value,
      });
      return value;
    },
    set(this: unknown, value: unknown): void {
      // An assignment to a writable data property changes it on `target`,
      // but makes an own property, enumerable, on any other receiver.
      ObjectDefineProperty(this as object, name, {
        __proto__: null,
        configurable: true,
        enumerable: this === target ? enumerable : true,
        writable: true,
        value,
      });
    },
  });
}

// The TypeScript HTMLRewriter replaces the C++ one, which global-scope.h then
// leaves out. Most Workers never use it, so its module is loaded on the first
// read of the global, after user code has run: the writer methods it captures
// from the streams globals above are captured now.
if (autogates['ts-html-rewriter']) {
  require('htmlrewriter/writer');
  const loadHTMLRewriter = (): unknown =>
    require('htmlrewriter/htmlrewriter').HTMLRewriter;
  // Installed as the MessageChannel classes above are, matching
  // JSG_NESTED_TYPE.
  defineLazyGlobal(
    ObjectGetPrototypeOf(globalThis),
    'HTMLRewriter',
    true,
    loadHTMLRewriter
  );
  if (compatFlags['spec_compliant_property_attributes']) {
    defineLazyGlobal(globalThis, 'HTMLRewriter', false, loadHTMLRewriter);
  }
}
