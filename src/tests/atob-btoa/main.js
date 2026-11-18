// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the atob() and btoa() suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test. A name two modules share
// is exported with the module's name before it.

export {
  globalPlacement,
  functionShape,
  notConstructors,
  sourceText,
  replaceable,
  sameFunctionEveryTime,
} from 'api-surface';

export {
  receiver,
  receiverIsCheckedFirst,
  missingArgument,
  undefinedIsConverted,
  primitives,
  objects,
  conversionOrder,
  symbols,
  unconvertibleObjects,
  conversionErrorsPropagate,
  extraArgumentsAreIgnored,
  stringsAreNotConverted,
} from 'conversions';

export {
  decodes,
  paddingIsOptional,
  asciiWhitespaceIsRemoved,
  discardsTrailingBits,
  rejects,
  twoByteStrings as decodingTwoByteStrings,
  stringShapes as decodingStringShapes,
  everyLength as decodingEveryLength,
  longInput as decodingLongInput,
  resultIsABinaryString,
  agreesWithTheSpecification,
} from 'decoding';

export {
  encodes,
  rejectsCodeUnitsAboveFF,
  twoByteStrings as encodingTwoByteStrings,
  stringShapes as encodingStringShapes,
  everyLength as encodingEveryLength,
  longInput as encodingLongInput,
  roundTrips,
} from 'encoding';

export {
  invalidCharacterError,
  typeErrors,
  freshErrorEachTime,
  stacksStartAtTheCaller,
  stackFramesOfThrownErrors,
  stackFramesDuringConversion,
  stackTraceLimit,
} from 'errors';

export {
  unreplaced,
  replacedGlobals,
  replacedStatics,
  replacedPrototypeMethods,
  objectPrototypeProperties,
  objectPrototypeAccessors,
  replacedFunctionsStillThrowTheirOwnErrors,
} from 'pollution';
