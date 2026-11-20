// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the navigator suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  globalPlacement,
  sameObjectEveryTime,
  replaceable,
  constructorShape,
  illegalConstructor,
  illegalConstructorStack,
  prototypeShape,
  instanceShape,
  toStringTag,
  sourceText,
  prototypeProperty,
  prototypeSwap,
  inspected,
} from 'api-surface';

export {
  values,
  readOnly,
  languagesIsANewArrayEachTime,
  storageIsTheSameObject,
  storageWorks,
  inherited,
  json,
  notSerializable,
} from 'attributes';

export {
  methodShape,
  receiver,
  receiverIsCheckedFirst,
  missingArgument,
  returnsTrueWithinARequest,
  returnsFalseOutsideARequest,
  urlIsConvertedToAString,
  symbols,
  conversionOrder,
  conversionErrorsPropagate,
  extraArgumentsAreIgnored,
  bodies,
  errorStacksStartAtTheCaller,
  stackFramesDuringConversion,
} from 'send-beacon';

export {
  unreplaced,
  replacedGlobals,
  replacedStatics,
  replacedPrototypeMethods,
  replacedMembers,
  objectPrototypeAccessors,
} from 'pollution';
