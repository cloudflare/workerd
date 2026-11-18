// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-337:
// Deserializer::ReadHostObject used obj.set() (ordinary [[Set]]) to restore
// serialized Error own-properties. A tenant-installed setter on
// Error.prototype would be invoked inside V8's DisallowJavascriptExecution
// scope, triggering V8_Fatal -> abort(). The fix uses CreateDataProperty
// which bypasses the prototype chain entirely.

import { strictEqual, ok } from 'node:assert';

export const errorDeserPrototypeSetterRegression = {
  test() {
    // Install a setter on Error.prototype for a key we will also define as
    // an own data property on the Error instance.
    let setterInvoked = false;
    Object.defineProperty(Error.prototype, 'evilprop', {
      set(_v) {
        setterInvoked = true;
      },
      get() {
        return undefined;
      },
      configurable: true,
    });

    try {
      const err = new Error('hello');
      // Define an own data property with the same key on the instance.
      Object.defineProperty(err, 'evilprop', {
        value: 42,
        enumerable: true,
        writable: true,
        configurable: true,
      });

      // Pre-patch this would abort the process with:
      //   V8 fatal error: Invoke in DisallowJavascriptExecutionScope
      const clone = structuredClone(err);

      // After fix: the own data property is recreated via
      // CreateDataProperty, the prototype setter is never invoked,
      // and the value round-trips.
      strictEqual(
        Object.getOwnPropertyDescriptor(clone, 'evilprop')?.value,
        42,
        'own data property evilprop should round-trip with value 42'
      );
      ok(
        !setterInvoked,
        'Error.prototype setter must not be invoked during deserialization'
      );
      strictEqual(clone.message, 'hello', 'error message should round-trip');
      ok(clone instanceof Error, 'clone should be an Error instance');
    } finally {
      // Clean up the prototype pollution.
      delete Error.prototype.evilprop;
    }
  },
};

// Also verify the serialization side, where the serializer copies own
// properties into a temporary plain object. With
// error_serialization_own_data_properties it defines them as data properties,
// so no Object.prototype setter runs and each property is kept. Without it,
// it assigns them, so the setter runs instead and the property is lost.
export const errorSerPrototypeSetterRegression = {
  test() {
    let setterInvoked = false;
    Object.defineProperty(Object.prototype, 'serprop', {
      set(_v) {
        setterInvoked = true;
      },
      get() {
        return undefined;
      },
      configurable: true,
    });

    try {
      const err = new Error('ser-test');
      Object.defineProperty(err, 'serprop', {
        value: 99,
        enumerable: true,
        writable: true,
        configurable: true,
      });

      // This exercises the serialization path where own properties are
      // copied to a temporary plain object.
      const clone = structuredClone(err);

      if (
        Cloudflare.compatibilityFlags.error_serialization_own_data_properties
      ) {
        strictEqual(
          Object.getOwnPropertyDescriptor(clone, 'serprop')?.value,
          99,
          'own data property serprop should round-trip with value 99'
        );
        ok(
          !setterInvoked,
          'Object.prototype setter must not be invoked during serialization'
        );
      } else {
        strictEqual(
          Object.getOwnPropertyDescriptor(clone, 'serprop'),
          undefined,
          'serprop should be assigned through the setter, not copied'
        );
        ok(
          setterInvoked,
          'Object.prototype setter must be invoked during serialization'
        );
      }
    } finally {
      delete Object.prototype.serprop;
    }
  },
};
