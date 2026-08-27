// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// API surface of IdentityTransformStream and FixedLengthStream: prototype
// shape, branding, and property behavior.
//
// Both test configs pin workers_api_getters_setters_on_prototype (so in both
// implementations readable/writable are enumerable get-accessors on a
// prototype) and set_tostring_tag (so both brand with Symbol.toStringTag).
// The remaining deliberate divergence is asserted per implementation below:
// - Accessor placement: C++ inherits readable/writable from
//   TransformStream.prototype; TypeScript defines them directly on
//   IdentityTransformStream.prototype. This is a consequence of the
//   inheritance divergence pinned by identityBrandChecks in construction.js:
//   only the C++ IdentityTransformStream is a TransformStream subclass.

import { ok, strictEqual, throws, match, doesNotMatch } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Walks the prototype chain and returns the first descriptor found for key,
// along with the object that holds it.
function findDescriptor(obj, key) {
  let o = Object.getPrototypeOf(obj);
  while (o !== null) {
    const desc = Object.getOwnPropertyDescriptor(o, key);
    if (desc !== undefined) return { desc, holder: o };
    o = Object.getPrototypeOf(o);
  }
  return undefined;
}

export const toStringTag = {
  test() {
    strictEqual(
      Object.prototype.toString.call(new IdentityTransformStream()),
      '[object IdentityTransformStream]'
    );
    strictEqual(
      Object.prototype.toString.call(new FixedLengthStream(0)),
      '[object FixedLengthStream]'
    );
  },
};

export const fixedLengthIsSubclassOfIdentity = {
  test() {
    const fls = new FixedLengthStream(10);
    ok(fls instanceof FixedLengthStream);
    ok(fls instanceof IdentityTransformStream);
    ok(new IdentityTransformStream() instanceof IdentityTransformStream);
  },
};

export const sidesAreStreamInstances = {
  test() {
    const { readable, writable } = new IdentityTransformStream();
    ok(readable instanceof ReadableStream);
    ok(writable instanceof WritableStream);
  },
};

export const readableWritableAreStable = {
  test() {
    // Repeated property access must yield the same object.
    const its = new IdentityTransformStream();
    strictEqual(its.readable, its.readable);
    strictEqual(its.writable, its.writable);
  },
};

export const propertyPlacement = {
  test() {
    const its = new IdentityTransformStream();
    for (const key of ['readable', 'writable']) {
      // Never an own property on the instance.
      strictEqual(Object.getOwnPropertyDescriptor(its, key), undefined);
      // An enumerable get-accessor somewhere on the prototype chain...
      const found = findDescriptor(its, key);
      ok(found, `No ${key} descriptor found on the prototype chain`);
      strictEqual(found.desc.enumerable, true);
      strictEqual(typeof found.desc.get, 'function');
      // ...whose holder diverges: TypeScript defines it directly on
      // IdentityTransformStream.prototype, C++ inherits it from
      // TransformStream.prototype.
      strictEqual(
        found.holder,
        usingTsImpl
          ? IdentityTransformStream.prototype
          : TransformStream.prototype
      );
    }
  },
};

export const constructorSourceText = {
  test() {
    // Which implementation is serving the classes is visible in their
    // source text: JSG-bound C++ constructors stringify as native code,
    // the TypeScript bootstrap's do not.
    for (const src of [
      String(IdentityTransformStream),
      String(FixedLengthStream),
    ]) {
      if (usingTsImpl) {
        doesNotMatch(src, /native code/);
      } else {
        match(src, /native code/);
      }
    }
  },
};

export const prototypeAccessorBrandChecks = {
  test() {
    const its = new IdentityTransformStream();
    const { desc } = findDescriptor(its, 'readable');
    // The accessor must brand-check its receiver.
    throws(() => desc.get.call({}), TypeError);
    throws(() => desc.get.call(null), TypeError);
  },
};
