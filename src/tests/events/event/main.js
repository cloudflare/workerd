// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the Event suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  toStringTagBranding,
  attributesArePrototypeAccessors,
  operationsArePrototypeMethods,
  isTrustedPlacement,
  phaseConstants,
  interfaceObjects,
  functionMetadata,
  brandChecks,
  constructorsRequireNew,
  internalClassMarker,
} from 'api-surface';

export {
  typeArgumentIsRequired,
  typeIsConvertedToString,
  initDictionaryConversion,
  initMembersReadOnceInOrder,
  initMembersAreConvertedToBoolean,
  initMembersAreIndependent,
  defaultAttributes,
  composedPathIsFresh,
  timeStampIsDateNowAtConstruction,
} from 'construction';

export {
  preventDefaultNeedsCancelable,
  returnValueSetterCancels,
} from 'cancelation';

export {
  stopPropagationSetsCancelBubble,
  cancelBubbleSetter,
  cancelBubbleFalseIsNoOp,
  stopImmediatePropagationSetsCancelBubble,
  setterArity,
} from 'propagation-flags';

export {
  initMethodsAreOperations,
  initEventReinitializes,
  initEventUnsetsStopFlags,
  initEventDuringDispatchIsNoOp,
  initEventArgumentConversion,
  initCustomEventSetsDetail,
} from 'init-event';

export {
  detailDefaultDiverges,
  detailIsReturnedAsGiven,
  customEventIsAnEvent,
} from 'custom-event';

export {
  subclassConstruction,
  subclassOverridesAnAttribute,
  isTrustedCannotBeRedefined,
  reflectConstructUsesNewTarget,
  customEventSubclass,
  subclassInstancesDispatch,
} from 'subclassing';

export {
  omittedInitReadsNothing,
  inheritedInitMembersAreRead,
  descriptorPollutionDoesNotReachIsTrusted,
  patchedBuiltinsAreNotUsed,
} from 'pollution';

export {
  structuredCloneDiverges,
  inspectDiverges,
  runtimeEventsAreCppEvents,
} from 'interop';
