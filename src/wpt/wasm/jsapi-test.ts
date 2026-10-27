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
    comment:
      'WebAssembly.compile() and instantiate() of bytes finish on a V8 foreground task, which workerd only runs when other work wakes the isolate, so these promises never settle (https://github.com/cloudflare/workerd/issues/7930)',
    disabledTests: [
      'Branding',
      'Empty buffer',
      'Invalid code',
      'Result type',
      'Stray argument',
      'Changing the buffer',
    ],
  },
  'constructor/instantiate-bad-imports.any.js': {
    comment:
      'WebAssembly.compile() and instantiate() of bytes finish on a V8 foreground task, which workerd only runs when other work wakes the isolate, so these promises never settle (https://github.com/cloudflare/workerd/issues/7930)',
    disabledTests: [/^WebAssembly\.instantiate\(buffer\): /],
  },
  'constructor/instantiate.any.js': {
    comment:
      'WebAssembly.compile() and instantiate() of bytes finish on a V8 foreground task, which workerd only runs when other work wakes the isolate, so these promises never settle (https://github.com/cloudflare/workerd/issues/7930)',
    disabledTests: [
      'Branding',
      /: BufferSource argument$/,
      'Synchronous options handling: Buffer argument',
      'Empty buffer',
      'Invalid code',
      'Changing the buffer',
    ],
  },
  'constructor/multi-value.any.js': {
    comment:
      'WebAssembly.compile() and instantiate() of bytes finish on a V8 foreground task, which workerd only runs when other work wakes the isolate, so these promises never settle (https://github.com/cloudflare/workerd/issues/7930)',
    disabledTests: true,
  },
  'constructor/toStringTag.any.js': {},
  'constructor/validate.any.js': {},
  'error-interfaces-no-symbol-tostringtag.js': {},
  'functions/helper.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'global/constructor.any.js': {},
  'global/toString.any.js': {},
  'global/value-get-set.any.js': {},
  'global/valueOf.any.js': {},
  'idlharness.any.js': {
    comment:
      'idl_test setup compiles /wasm/incrementer.wasm in a promise continuation, where workerd disallows Wasm code generation; where allowed, the compile would never settle (https://github.com/cloudflare/workerd/issues/7930). ' +
      'V8 makes WebAssembly.Module extend AbstractModuleSource (source phase imports), which wasm-js-api.idl does not describe (https://github.com/cloudflare/workerd/issues/7931)',
    expectedFailures: [
      'idl_test setup',
      'Module interface: existence and properties of interface object',
      'Module interface: existence and properties of interface prototype object',
    ],
  },
  'instance/constructor-bad-imports.any.js': {},
  'instance/constructor-caching.any.js': {},
  'instance/constructor.any.js': {},
  'instance/exports.any.js': {},
  'instance/toString.any.js': {},
  'instanceTestFactory.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
  'interface.any.js': {},
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
  'memory/buffer.any.js': {},
  'memory/constructor.any.js': {},
  'memory/grow.any.js': {},
  'memory/to-fixed-length-buffer-shared.any.js': {},
  'memory/to-fixed-length-buffer.any.js': {},
  'memory/to-resizable-buffer-shared.any.js': {},
  'memory/to-resizable-buffer.any.js': {},
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
  'table/get-set.any.js': {},
  'table/grow.any.js': {},
  'table/length.any.js': {},
  'table/toString.any.js': {},
  'wasm-module-builder.js': {
    comment: 'Helper script loaded by other tests',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
