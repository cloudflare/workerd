// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* global QuotaExceededError */

// Entry point for the DOMException suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

import { WorkerEntrypoint } from 'cloudflare:workers';
import { queued } from 'serialization';

export {
  constructorShape,
  prototypeShape,
  attributeAccessors,
  legacyCodeConstants,
  toStringTag,
  instanceShape,
  subclassing,
  globalPlacement,
} from 'api-surface';

export {
  constructorRequiresNew,
  defaults,
  argumentConversion,
  argumentConversionOrder,
  codes,
  loneSurrogates,
} from 'construction';

export {
  isAnError,
  objectToString,
  errorToString,
  attributesAreBrandChecked,
  attributesAreReadOnly,
  ownPropertiesShadowAttributes,
  deletingAttributes,
  frozen,
} from 'errors';

export {
  stackHeaderAndFrames,
  subclassFrames,
  stackProperty,
  prepareStackTrace,
  captureStackTrace,
} from 'stack';

export {
  jsonStringify,
  enumerableKeys,
  addedPropertiesEnumerate,
} from 'enumeration';

export {
  fromATunneledError,
  fromAnAsyncTunneledError,
  fromTheSerializer,
  abortReasons,
  fromTheFileSystem,
} from 'native';

export {
  structuredCloneRoundTrip,
  nativeInstancesKeepTheirStack,
  subclassesCloneAsDOMException,
  onlyNameMessageAndStackAreCloned,
  nestedAndPosted,
  readsTag8,
  readsTag7,
  readsUtf8,
} from 'serialization';

export {
  thrownFromAFetchHandler,
  thrownOverRpc,
  returnedOverRpc,
  abortReasonsOverRpc,
  erroredStreams,
} from 'tunneling';

export { inspectInstance, inspectNested, inspectPrototype } from 'inspect';

export {
  quotaExceededErrorPlacement,
  quotaExceededErrorShape,
  quotaExceededErrorInstances,
  quotaExceededErrorConversion,
  quotaExceededErrorRangeErrors,
  quotaExceededErrorBrandChecks,
  quotaExceededErrorCloned,
  quotaExceededErrorOverRpc,
} from 'quota-exceeded-error';

// The RPC binding's entrypoint (tunneling.js).
export class DomExceptionRpc extends WorkerEntrypoint {
  throwDomException() {
    throw new DOMException('thrown', 'NotFoundError');
  }

  describeAbortReason(signal) {
    const { reason } = signal;
    return [
      reason instanceof DOMException,
      reason.name,
      reason.message,
      reason.code,
      typeof reason.stack,
    ];
  }

  returnDomException() {
    const error = new DOMException('returned', 'AbortError');
    error.extra = 1;
    return error;
  }

  // The rest are only for the TypeScript implementation's QuotaExceededError
  // (quota-exceeded-error.js).
  returnQuotaExceededError() {
    return new QuotaExceededError('returned', { quota: 1, requested: 2 });
  }

  describeQuotaExceededError(error) {
    return [
      error instanceof QuotaExceededError,
      error.message,
      error.quota,
      error.requested,
    ];
  }

  throwQuotaExceededError() {
    throw new QuotaExceededError('thrown', { quota: 1 });
  }
}

export default {
  // The SELF binding's handlers: fetch() throws (tunneling.js); queue()
  // records the bodies of crafted payloads (serialization.js).
  fetch() {
    throw new DOMException('thrown', 'InvalidStateError');
  },

  queue(batch) {
    for (const message of batch.messages) {
      queued.set(message.id, message.body);
    }
    batch.ackAll();
  },
};
