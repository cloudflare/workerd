// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DOMException as an error: what identifies it as one, how it renders as
// a string, the attributes' brand checks, and writes to its attributes.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl, setToStringTag } from 'which-impl';

export const isAnError = {
  test() {
    const error = new DOMException('m', 'AbortError');
    strictEqual(error instanceof Error, true);
    // Ledger #3: WebIDL gives DOMException [[ErrorData]].
    strictEqual(Error.isError(error), usingTsImpl);
    strictEqual(Error.isError(DOMException.prototype), false);
  },
};

export const objectToString = {
  test() {
    const error = new DOMException('m', 'AbortError');
    // Ledger #4: with no Symbol.toStringTag, [[ErrorData]] makes it an
    // Error.
    let expected = '[object Object]';
    if (setToStringTag) expected = '[object DOMException]';
    else if (usingTsImpl) expected = '[object Error]';
    strictEqual(Object.prototype.toString.call(error), expected);
  },
};

export const errorToString = {
  test() {
    strictEqual(String(new DOMException('m', 'AbortError')), 'AbortError: m');
    strictEqual(String(new DOMException('', 'AbortError')), 'AbortError');
    strictEqual(String(new DOMException('m')), 'Error: m');
    strictEqual(
      Error.prototype.toString.call(new DOMException('m', 'NotFoundError')),
      'NotFoundError: m'
    );
  },
};

export const attributesAreBrandChecked = {
  test() {
    const notADomException = Object.create(DOMException.prototype);
    if (usingTsImpl) {
      // Ledger #7: the getters throw for anything but a DOMException.
      for (const attribute of ['name', 'message', 'code']) {
        throws(() => notADomException[attribute], TypeError);
        const { get } = Object.getOwnPropertyDescriptor(
          DOMException.prototype,
          attribute
        );
        for (const receiver of [
          {},
          new Error('m'),
          DOMException.prototype,
          undefined,
          null,
        ]) {
          throws(() => get.call(receiver), TypeError);
        }
      }
      throws(() => String(DOMException.prototype), TypeError);
      throws(() => String(notADomException), TypeError);
    } else {
      // Error.prototype's.
      strictEqual(notADomException.name, 'Error');
      strictEqual(notADomException.message, '');
      strictEqual(notADomException.code, undefined);
      strictEqual(String(DOMException.prototype), 'Error');
    }
    strictEqual(notADomException instanceof DOMException, true);
  },
};

export const attributesAreReadOnly = {
  test() {
    'use strict';
    const error = new DOMException('m', 'AbortError');
    for (const attribute of ['name', 'message', 'code']) {
      throws(() => {
        error[attribute] = 'x';
      }, TypeError);
    }
    strictEqual(error.name, 'AbortError');
    strictEqual(error.message, 'm');
    strictEqual(error.code, 20);
  },
};

export const ownPropertiesShadowAttributes = {
  test() {
    const error = new DOMException('m', 'AbortError');
    Object.defineProperty(error, 'name', { value: 'Other' });
    Object.defineProperty(error, 'message', { value: 'other' });
    strictEqual(error.name, 'Other');
    strictEqual(error.message, 'other');
    strictEqual(String(error), 'Other: other');
  },
};

export const deletingAttributes = {
  test() {
    const error = new DOMException('m', 'AbortError');
    strictEqual(delete error.name, true);
    strictEqual(delete error.message, true);
    strictEqual(delete error.code, true);
    if (usingTsImpl) {
      // Ledger #1: nothing own to delete.
      strictEqual(error.name, 'AbortError');
      strictEqual(error.message, 'm');
      strictEqual(error.code, 20);
    } else {
      // Error.prototype's again.
      strictEqual(error.name, 'Error');
      strictEqual(error.message, '');
      strictEqual(error.code, undefined);
    }
  },
};

export const frozen = {
  test() {
    const error = Object.freeze(new DOMException('m', 'AbortError'));
    strictEqual(Object.isFrozen(error), true);
    strictEqual(error.name, 'AbortError');
    strictEqual(error.code, 20);
  },
};
