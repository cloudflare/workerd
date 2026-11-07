// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// DOMExceptions that native code creates are instances of the global
// DOMException, shaped as the constructor shapes them, whichever path made
// them: a tunneled JSG error, the serializer, AbortSignal, Lock's
// domException().

import { rejects, strictEqual } from 'node:assert';
import { assertDomException } from 'helpers';

function caught(fn) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  throw new Error('did not throw');
}

export const fromATunneledError = {
  test() {
    // JSG_REQUIRE(..., DOMInvalidCharacterError, ...).
    assertDomException(
      caught(() => atob('!')),
      {
        name: 'InvalidCharacterError',
        message:
          "atob() called with invalid base64-encoded data. (Only whitespace, '+', '/', " +
          "alphanumeric ASCII, and up to two terminal '=' signs when the input data " +
          'length is divisible by 4 are allowed.)',
        code: 5,
      }
    );
  },
};

export const fromAnAsyncTunneledError = {
  async test() {
    let error;
    try {
      await crypto.subtle.importKey(
        'raw',
        new Uint8Array(3),
        'AES-GCM',
        false,
        ['encrypt']
      );
    } catch (e) {
      error = e;
    }
    assertDomException(error, {
      name: 'DataError',
      message:
        'Imported AES key length must be 128, 192, or 256 bits but provided 24.',
      code: 0,
    });
  },
};

export const fromTheSerializer = {
  test() {
    assertDomException(
      caught(() => structuredClone(() => {})),
      {
        name: 'DataCloneError',
        message: '() => {} could not be cloned.',
        code: 25,
      }
    );
    assertDomException(
      caught(() => structuredClone(new TextEncoder())),
      {
        name: 'DataCloneError',
        message:
          'Could not serialize object of type "TextEncoder". This type does not ' +
          'support serialization.',
        code: 25,
      }
    );
  },
};

export const abortReasons = {
  async test() {
    // The default reasons are retryable: exceptionToJs() adds the property.
    const controller = new AbortController();
    controller.abort();
    const aborted = {
      name: 'AbortError',
      message: 'The operation was aborted',
      code: 20,
    };
    assertDomException(controller.signal.reason, aborted, 'retryable');
    assertDomException(AbortSignal.abort().reason, aborted, 'retryable');
    strictEqual(controller.signal.reason.retryable, true);

    const signal = AbortSignal.timeout(1);
    await new Promise((resolve) => signal.addEventListener('abort', resolve));
    assertDomException(
      signal.reason,
      {
        name: 'TimeoutError',
        message: 'The operation was aborted due to timeout',
        code: 23,
      },
      'retryable'
    );
  },
};

export const fromTheFileSystem = {
  async test() {
    // filesystem.c++ creates these with Lock::domException() directly.
    const root = await navigator.storage.getDirectory();
    await rejects(root.getFileHandle('missing'), (error) => {
      assertDomException(error, {
        name: 'NotFoundError',
        message: 'Not found',
        code: 8,
      });
      return true;
    });
  },
};
