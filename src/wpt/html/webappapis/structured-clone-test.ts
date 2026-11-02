// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'structured-clone-battery-of-tests-harness.js': {
    comment: 'Helper file loaded by structured-clone.any.js, not a test',
    omittedTests: true,
  },
  'structured-clone-battery-of-tests-with-transferables.js': {
    comment: 'Helper file loaded by structured-clone.any.js, not a test',
    omittedTests: true,
  },
  'structured-clone-battery-of-tests.js': {
    comment: 'Helper file loaded by structured-clone.any.js, not a test',
    omittedTests: true,
  },
  'structured-clone.any.js': {
    comment:
      'See the comment above each group. ImageBitmap and OffscreenCanvas are canvas APIs that workerd does not have.',
    expectedFailures: [
      // enhanced_error_serialization deliberately clones custom own properties of errors and
      // gives an empty Error an own `message`, which the spec says must not survive a clone.
      'Empty Error object',
      'Error object',
      'EvalError object',
      'RangeError object',
      'ReferenceError object',
      'SyntaxError object',
      'TypeError object',
      'URIError object',
      // File is not serializable, although Blob is.
      'File basic',
      'A subclass instance will deserialize as its closest serializable superclass',
      // structuredClone(new Response()) succeeds: types that are JSG_SERIALIZABLE for RPC
      // (Request, Response) are also accepted by structuredClone, which should throw a
      // DataCloneError.
      'Serializing a non-serializable platform object fails',
      // The test compares the clone with compare_ArrayBuffer, which asserts
      // `instanceof ArrayBuffer`; a SharedArrayBuffer never satisfies that. Browsers only pass
      // because they reject the clone when not cross-origin isolated, while workerd clones it.
      'Growable SharedArrayBuffer',
      // structuredClone can only transfer ArrayBuffers; MessagePort and ReadableStream
      // transfers throw TypeError "Object is not transferable" (jsg/ser.c++).
      'MessagePort',
      'A detached platform object cannot be transferred',
      'An object whose interface is deleted from the global object must still be received',
      'A subclass instance will be received as its closest transferable superclass',
      // Transferring a Blob throws TypeError rather than a DataCloneError.
      'Transferring a non-transferable platform object fails',
      // Transferring an already-detached ArrayBuffer succeeds instead of throwing a
      // DataCloneError.
      'A detached ArrayBuffer cannot be transferred',
    ],
    omittedTests: ['ImageBitmap', 'OffscreenCanvas'],
  },
} satisfies TestRunnerConfig;
