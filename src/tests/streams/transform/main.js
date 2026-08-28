// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the transform streams suite. Re-exports are explicit
// (not `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  transformGlobalsExist,
  controllerNotConstructable,
  defaultIdentityTransform,
  savedControllerRetainsStream,
} from 'api-surface';

export {
  simpleTransform,
  delayTransform,
  differentTypesTransform,
} from 'transformer-algorithms';

export {
  syncErrorDuringStart,
  asyncErrorDuringStart,
  syncErrorDuringTransform,
  asyncErrorDuringTransform,
  syncErrorDuringFlush,
  asyncErrorDuringFlush,
  errorInTransformFlush,
} from 'error-propagation';

export {
  writeBackpressure,
  backpressureTransformBothStrategies,
} from 'backpressure';

export {
  sizeCallbackErrorDoesNotUAF,
  sizeCallbackErrorAndThrowDoesNotUAF,
  sizeCallbackErrorSequential,
} from 'reentrancy';

export { transformRoundtrip } from 'roundtrip';
