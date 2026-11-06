// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the URL suite. Re-exports are explicit (not `export *`)
// so a name collision between modules is a load-time SyntaxError instead
// of a silently dropped test.

export {
  urlPrototypeMembers,
  urlMemberAttributes,
  urlStatics,
  searchParamsPrototypeMembers,
  constructorAttributes,
  constructorNamesAndSpread,
  legacyInstanceProperties,
  legacyInstancePropertiesShadowSubclasses,
  toStringTagBranding,
  runtimeClassMarks,
  globalsPlacement,
  nodeUrlHandsOutTheGlobals,
} from 'api-surface';

export {
  urlMembersAreBrandChecked,
  searchParamsMembersAreBrandChecked,
  iteratorNextIsBrandChecked,
  staticsTakeAnyReceiver,
} from 'illegal-invocation';

export {
  parseFailures,
  baseHandling,
  urlRequiresAnArgument,
  searchParamsArgumentIsOptional,
  constructorsRequireNew,
  subclassing,
  statics,
  objectUrlsAreUnsupported,
} from 'construction';

export {
  setterResults,
  hrefSetterThrows,
  readonlyAttributes,
  setterValuesAreConverted,
  legacyStringifyErrors,
} from 'setters';

export {
  originValues,
  originFollowsEverySetter,
  originAfterSearchParamsMutation,
} from 'origin-cache';

export {
  searchParamsIsTheSameObject,
  urlSettersReachSearchParams,
  searchParamsMutatorsReachUrl,
  noOpMutatorsStillSerialize,
  searchParamsCopyIsIndependent,
  searchGetterEncoding,
} from 'search-params-coupling';

export {
  sequenceInitializers,
  badSequences,
  recordInitializers,
  recordReadOrder,
  stringInitializers,
  listOperations,
  deleteAndHasWithAValue,
  sortIsStableByCodeUnits,
  serialization,
  missingArgumentsThrow,
} from 'search-params';

export {
  iteratorPrototypes,
  defaultIterator,
  iteratorResults,
  iterationIsLive,
  iteratorOverUrlSearchParams,
  forEachCallback,
  forEachReceiver,
  forEachIsLive,
  forEachPropagatesThrows,
} from 'iteration';

export {
  conversionOrder,
  symbolsThrow,
  loneSurrogates,
  recordKeysAreUsvStrings,
} from 'conversions';

export {
  inspectUrl,
  inspectSearchParams,
  inspectIterator,
  inspectPrototype,
} from 'inspect';

export {
  structuredCloneThrows,
  postMessageThrows,
  subclassInstancesThrow,
} from 'serialization';

export {
  searchParamsBodies,
  urlsAsStrings,
  nodeFsPaths,
  urlPatternFromUrl,
} from 'consumers';

export { builtinMethodsAreNotCalled, resultsRunNoSetters } from 'pollution';

export {
  searchParamsKeepsItsUrlAlive,
  iteratorKeepsItsParamsAlive,
  unreachablePairsAreCollected,
} from 'gc';

export {
  legacyConstructorCases,
  legacySetterCases,
  legacyWptConstructorCases,
  legacyWptSetterCases,
} from 'legacy-conformance';
