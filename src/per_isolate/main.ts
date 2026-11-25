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

// ***WARNING***: Nothing that runs in main.ts top-level scope should be
// gated on compat flag. The top-level scope is executed while building
// the zygote snapshot. Depending on autogates here is fine, anything
// that requires compat flag gating must be handled in main-post.ts.
// It is ok for things here to capture compatFlags for lazy usage after
// the bootstrap runs, so we haven't removed it entirely from this scope.
// The same holds for the top level of every module main.ts loads, and for
// a check whether a global exists that only a flag installs.

const { ObjectDefineProperties, ObjectGetPrototypeOf } = primordials;

const globalPrototype = ObjectGetPrototypeOf(globalThis) as object;

require('serialization');

if (autogates['ts-performance-now']) {
  require('performance/now').install(utils.getPerformancePrototype());
}

if (autogates['ts-web-streams']) {
  require('webstreams/installer').installWebStreams();
}

// The TypeScript structuredClone() replaces the C++ method, which
// global-scope.h then leaves out. Installed where JSG_METHOD installs it, on
// ServiceWorkerGlobalScope.prototype, with the same attributes (all three
// true).
if (autogates['ts-structured-clone']) {
  const { structuredClone } = require('structured-clone');
  ObjectDefineProperties(globalPrototype, {
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
