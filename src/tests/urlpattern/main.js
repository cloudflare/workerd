// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the URLPattern suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  prototypeMembers,
  memberAttributes,
  constructorAttributes,
  instancesHaveNoOwnProperties,
  subclassing,
  toStringTagBranding,
  runtimeClassMarks,
  globalPlacement,
} from 'api-surface';

export { membersAreBrandChecked } from 'illegal-invocation';

export {
  constructorRequiresNew,
  defaults,
  patternStrings,
  initPatterns,
  overloads,
  threeArgumentOverload,
  optionsArgument,
  nonObjectInputsAreStrings,
  parseErrors,
  regExpFlags,
  hasRegExpGroups,
} from 'construction';

export {
  initReadOrder,
  initMembersAreStrings,
  argumentConversionOrder,
  optionsReadOnlyIgnoreCase,
  loneSurrogates,
} from 'conversions';

export {
  stringInputs,
  unparseableStringInputs,
  initInputs,
  initInputsThatDoNotProcess,
  initInputsWithABaseUrl,
  componentMatching,
  ignoreCase,
} from 'matching';

export {
  resultShape,
  groups,
  namedFullWildcard,
  unmatchedOptionalGroups,
  stringInputsAreInputs,
  initInputsAreCopied,
} from 'results';

export {
  regExpExecIsNotCalled,
  constructionCallsPatchedExecForProtocol,
  groupsIgnoreObjectPrototypeSetters,
  resultsIgnoreObjectPrototypeSetters,
  inputsIgnoreArrayPrototypeSetters,
} from 'pollution';

export { inspectPattern, inspectPrototype } from 'inspect';

export {
  structuredCloneThrows,
  postMessageThrows,
  resultsAreCloneable,
} from 'serialization';
