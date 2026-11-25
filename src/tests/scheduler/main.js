// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the scheduler suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time SyntaxError
// instead of a silently dropped test.

export {
  globalPlacement,
  sameObjectEveryTime,
  replaceable,
  constructorShape,
  illegalConstructor,
  illegalConstructorStack,
  prototypeShape,
  instanceShape,
  waitShape,
  toStringTag,
  sourceText,
  prototypeProperty,
  inspected,
  notSerializable,
} from 'shape';

export {
  receiver,
  receiverErrorStack,
  failureStack,
  resolves,
  delayConversion,
  optionsConversion,
  conversionOrder,
  alreadyAborted,
  abortedWhileWaiting,
  abortOrdering,
  abortAfterTheWait,
  timerOrder,
  replacedBuiltIns,
  outsideOfARequest,
} from 'wait';
