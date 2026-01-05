// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'ecmascript-binding/builtin-function-properties.any.js': {},
  'ecmascript-binding/class-string-interface.any.js': {
    comment: '@@toStringTag property descriptor mismatch',
    expectedFailures: [
      '@@toStringTag exists on the prototype with the appropriate descriptor',
    ],
  },
  'ecmascript-binding/class-string-iterator-prototype-object.any.js': {
    comment: 'Iterator @@toStringTag is different in workerd',
    expectedFailures: [
      '@@toStringTag exists with the appropriate descriptor',
      'Object.prototype.toString',
      'Object.prototype.toString applied to a null-prototype instance',
    ],
  },
  'ecmascript-binding/class-string-named-properties-object.window.js': {
    comment: 'Window-specific test',
    omittedTests: true,
  },
  'ecmascript-binding/es-exceptions/DOMException-constants.any.js': {
    comment: 'DOMException constants have wrong configurable descriptor',
    expectedFailures: true,
  },
  'ecmascript-binding/es-exceptions/DOMException-constructor-and-prototype.any.js':
    {
      comment: 'DOMException property descriptors differ from spec',
      expectedFailures: true,
    },
  'ecmascript-binding/es-exceptions/DOMException-constructor-behavior.any.js': {
    comment: 'DOMException property inheritance differs from spec',
    expectedFailures: true,
  },
  'ecmascript-binding/es-exceptions/DOMException-custom-bindings.any.js': {
    comment: 'DOMException property descriptors differ from spec',
    expectedFailures: true,
  },
  'ecmascript-binding/es-exceptions/DOMException-is-error.any.js': {
    comment: 'DOMException.prototype not in Error.prototype chain',
    expectedFailures: [''],
  },
  'ecmascript-binding/global-immutable-prototype.any.js': {
    comment: 'globalThis prototype is unconfigurable in workerd',
    expectedFailures: true,
  },
  'ecmascript-binding/global-mutable-prototype.any.js': {
    comment: 'globalThis prototype is unconfigurable in workerd',
    expectedFailures: true,
  },
  'ecmascript-binding/global-object-implicit-this-value.any.js': {
    comment:
      'Script fails to load - addEventListener is not defined in workerd',
    expectedFailures: [
      "Global object's getter throws when called on incompatible object",
      "Global object's setter throws when called on incompatible object",
      "Global object's operation throws when called on incompatible object",
      "Global object's getter works when called on null / undefined",
      "Global object's setter works when called on null / undefined",
      "Global object's operation works when called on null / undefined",
    ],
  },
  'ecmascript-binding/legacy-factor-function-subclass.window.js': {
    comment: 'Window-specific test',
    omittedTests: true,
  },
  'ecmascript-binding/legacy-factory-function-builtin-properties.window.js': {
    comment: 'Window-specific test',
    omittedTests: true,
  },
  'ecmascript-binding/legacy-platform-object/helper.js': {},
  'ecmascript-binding/no-regexp-special-casing.any.js': {
    comment: 'self.addEventListener is not available in workerd',
    expectedFailures: [
      'Conversion to a dictionary works',
      'Conversion to a sequence works',
      'Can be used as an object implementing a callback interface',
    ],
  },
  'ecmascript-binding/observable-array-no-leak-of-internals.window.js': {
    comment: 'Window-specific test',
    omittedTests: true,
  },
  'ecmascript-binding/observable-array-ownkeys.window.js': {
    comment: 'Window-specific test',
    omittedTests: true,
  },
  'ecmascript-binding/support/create-realm.js': {},
  'idlharness.any.js': {
    comment: 'Missing /resources/WebIDLParser.js resource file',
    omittedTests: true,
  },
} satisfies TestRunnerConfig;
