'use strict';
// Entry point of the test embedder's bootstrap scripts. Records what it can
// see for per-isolate-embedder-bootstrap-test.c++ to check.

const { ObjectDefineProperty } = primordials;

// A workerd bootstrap script, resolved by the embedder's require().
const { RingBuffer } = require('webstreams/ring-buffer');

ObjectDefineProperty(globalThis, 'testEmbedderGlobal', {
  __proto__: null,
  configurable: true,
  enumerable: false,
  writable: true,
  value: 'installed',
});

module.exports = {
  helper: require('test-embedder/helper'),
  // workerd's main has already run, so this is its cached exports object.
  workerdMain: require('main'),
  ringBufferLength: new RingBuffer().length,
  // The WeakRef globals are removed only after the embedder's scripts run.
  weakRefType: typeof WeakRef,
  hasUtils: typeof utils.isPromise === 'function',
  sawCompatFlag: compatFlags['typescript_implemented_streams'] === true,
  requireLater: (specifier: string): unknown => require(specifier),
};
