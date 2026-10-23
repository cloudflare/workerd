// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'assertions.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'bad-imports.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'constructor/compile.any.js': {
    comment: 'Hangs until the runtime cancels the request; needs investigation',
    disabledTests: true,
  },
  'constructor/instantiate-bad-imports.any.js': {
    comment: 'Hangs until the runtime cancels the request; needs investigation',
    disabledTests: true,
  },
  'constructor/instantiate.any.js': {
    comment: 'Hangs until the runtime cancels the request; needs investigation',
    disabledTests: true,
  },
  'constructor/multi-value.any.js': {
    comment: 'Hangs until the runtime cancels the request; needs investigation',
    disabledTests: true,
  },
  'constructor/toStringTag.any.js': {},
  'constructor/validate.any.js': {},
  'error-interfaces-no-symbol-tostringtag.js': {},
  'functions/helper.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'global/constructor.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which throws on BigInt values",
    disabledTests: true,
  },
  'global/toString.any.js': {},
  'global/value-get-set.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'global/valueOf.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'idlharness.any.js': {
    comment: 'Loads resources/load_wasm.js, which the harness does not bind',
    disabledTests: true,
  },
  'instance/constructor-bad-imports.any.js': {},
  'instance/constructor-caching.any.js': {},
  'instance/constructor.any.js': {
    comment:
      "workerd's WebAssembly.Instance shim (io/worker.c++) can be called without new",
    expectedFailures: ['Calling'],
  },
  'instance/exports.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'instance/toString.any.js': {},
  'instanceTestFactory.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'interface.any.js': {
    comment:
      "workerd's WebAssembly.Instance shim (io/worker.c++) has a writable prototype property",
    expectedFailures: ['WebAssembly.Instance: prototype'],
  },
  'js-string/basic.any.js': {},
  'js-string/constants.any.js': {},
  'js-string/imports.any.js': {},
  'js-string/polyfill.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'jspi/js-promise-integration.any.js': {
    comment:
      'Compiles a module in a promise continuation, where workerd disallows Wasm code generation',
    expectedFailures: ['Suspending with mismatched args and via Proxy'],
  },
  'jspi/notraps.any.js': {},
  'jspi/rejects.any.js': {},
  'jspi/testharness-additions.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'memory/assertions.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'memory/buffer.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'memory/constructor.any.js': {},
  'memory/grow.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'memory/to-fixed-length-buffer-shared.any.js': {},
  'memory/to-fixed-length-buffer.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['API surface'],
  },
  'memory/to-resizable-buffer-shared.any.js': {},
  'memory/to-resizable-buffer.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['API surface'],
  },
  'memory/toString.any.js': {},
  'module/constructor.any.js': {},
  'module/customSections.any.js': {},
  'module/exports.any.js': {},
  'module/imports.any.js': {},
  'module/toString.any.js': {},
  'prototypes.any.js': {},
  'table/assertions.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'table/constructor.any.js': {},
  'table/get-set.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding: get', 'Branding: set'],
  },
  'table/grow.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'table/length.any.js': {
    comment:
      "The harness's format_value() calls JSON.stringify(), which runs the prototype's getters and throws",
    expectedFailures: ['Branding'],
  },
  'table/toString.any.js': {},
  'wasm-module-builder.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
