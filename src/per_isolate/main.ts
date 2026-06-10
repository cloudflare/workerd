// Per-isolate bootstrap entry point.
// This script runs synchronously at context creation time,
// before any user code.

// This bootstrap script is called when a new context for an isolate is created.
// It is used to install any necessary global objects or functions that should
// be available at the global scope implemented in JavaScript/TypeScript. This
// runs before any user code is executed.
//
// There are six pseudo-globals available in this script:
// - `globalThis`: the global object of the context being initialized.
// - `compatFlags`: an object containing compatibility flags for the context.
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
