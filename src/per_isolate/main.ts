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
const { ObjectDefineProperties, ObjectGetPrototypeOf } = primordials;

// Loaded whatever the compatibility flags, as is every module that sets up a
// cloneable class that every reader must be able to read (see
// serialization.ts).
require('serialization');

// performance.now() and performance.timeOrigin. Gated by an autogate rather
// than a compat flag because the swap preserves the observable shape (see
// performance/AGENTS.md).
if (autogates['ts-performance-now']) {
  require('performance/now').install(utils.getPerformancePrototype());
}

if (autogates['ts-web-streams']) {
  const { installWebStreams } = require('webstreams/installer');
  installWebStreams();
}
// TODO(soon): Note that while the TS web streams are still compat flag
// gated, nothing in here can eagerly depend on the web streams impl,
// as it might not get installed until main-post.

// MessageChannel and MessagePort are loaded on first use, since few Workers
// use them: through the globals defined here, or through
// cloudflare-internal:messagechannel (MessageChannelModule in
// api/messagechannel.h), which node:worker_threads uses. Whichever comes first
// loads the module; the other finds it in the require() cache.
if (compatFlags['expose_global_message_channel']) {
  const { defineLazyDataProperty } = utils;
  const specifier = 'messagechannel/messagechannel';
  // Defined as JSG_NESTED_TYPE defines a global class: always on the global
  // scope's prototype, with all three attributes true, and under
  // spec_compliant_property_attributes also as own properties of the global,
  // non-enumerable per Web IDL. Each is a lazy data property, an ordinary data
  // property once read.
  const globalPrototype = ObjectGetPrototypeOf(globalThis) as object;
  defineLazyDataProperty(globalPrototype, 'MessageChannel', specifier, true);
  defineLazyDataProperty(globalPrototype, 'MessagePort', specifier, true);
  if (compatFlags['spec_compliant_property_attributes']) {
    defineLazyDataProperty(globalThis, 'MessageChannel', specifier, false);
    defineLazyDataProperty(globalThis, 'MessagePort', specifier, false);
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

// The TypeScript structuredClone() replaces the C++ method, which
// global-scope.h then leaves out. Installed where JSG_METHOD installs it, on
// ServiceWorkerGlobalScope.prototype, with the same attributes (all three
// true).
if (autogates['ts-structured-clone']) {
  const { structuredClone } = require('structured-clone');
  ObjectDefineProperties(ObjectGetPrototypeOf(globalThis), {
    __proto__: null,
    structuredClone: {
      __proto__: null,
      configurable: true,
      enumerable: true,
      writable: true,
      value: structuredClone,
    },
  });
}

// Internal-only: a cloneable class for testing the serialization protocol
// (serialization.ts), such as over RPC. Gated by an experimental flag that
// may never lose its experimental annotation.
if (compatFlags['expose_serialization_test_class']) {
  const { CloneableTestBox } = require('testing/cloneable-test-box');
  ObjectDefineProperties(globalThis, {
    CloneableTestBox: {
      __proto__: null,
      configurable: true,
      enumerable: false,
      writable: true,
      value: CloneableTestBox,
    },
  });
}
