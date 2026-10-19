// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the EventTarget suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  toStringTagBranding,
  methodsArePrototypeMethods,
  functionMetadata,
  interfaceObject,
  brandChecks,
  detachedMethodsThrow,
  internalClassMarker,
} from 'api-surface';

export {
  argumentCounts,
  argumentsAreConvertedInOrder,
  nullishListenerIsIgnored,
  primitiveListenersThrow,
  nonCallableObjectListener,
  duplicatesAreIgnored,
  removalMatchesByIdentity,
  listenersRunInRegistrationOrder,
  listenerInvocation,
  typeIsConvertedToString,
  registrationsArePerTarget,
} from 'listeners';

export {
  inertOptions,
  captureOption,
  captureListenersRunFirst,
  captureIsPartOfIdentity,
  stopPropagationInCaptureSkipsBubbling,
  listenerAddedDuringCaptureRunsInBubbling,
  passiveListenerCannotCancel,
  passiveIsNotPartOfIdentity,
  passiveFlagResetsAfterAThrow,
  optionsReadOrder,
  onceRemovesBeforeInvoking,
  onceIsConvertedToBoolean,
  onceListenerRemovedEarlierDoesNotRun,
  removeWithCapture,
} from 'options';

export {
  argumentMustBeAnEvent,
  attributesDuringDispatch,
  attributesAfterDispatch,
  dispatchResult,
  canceledFlagSurvivesDispatch,
  stopFlagsSetBeforeDispatch,
  stopFlagsSetDuringDispatch,
  removedListenerDoesNotRun,
  addedListenerWaitsForTheNextDispatch,
  reAddedListenerDuringDispatch,
  reentrantDispatchThrows,
  nestedDispatch,
  redispatchAtAnotherTarget,
  unusualTypes,
} from 'dispatch';

export {
  handleEventLookup,
  handleEventReplacedAfterAdd,
  throwingHandleEventGetter,
  nonCallableHandleEventIsReported,
  callableListenerIgnoresHandleEvent,
  inheritedHandleEvent,
  listenerReturningTrue,
  otherResultsAreIgnored,
} from 'handle-event';

export {
  abortRemovesTheListener,
  abortedSignalAddsNothing,
  abortRemovesOnlyItsOwnRegistration,
  duplicateIgnoresItsSignal,
  oneSignalForManyListeners,
  signalWithOnce,
  removedBeforeTheAbortEvent,
  syntheticAbortEventRemovesNothing,
  abortDuringDispatch,
  signalMustBeAnAbortSignal,
  forgedSignal,
} from 'signal';

export {
  exceptionsAreReported,
  everyExceptionIsReported,
  throwingErrorListener,
  reportRunsBetweenListeners,
  throwingOnceListenerIsRemoved,
  stateAfterAnException,
  replacedReportErrorIsNotCalled,
  unloggableException,
  exceptionsOutsideARequest,
} from 'exceptions';

export {
  onTypeProperty,
  onTypeMethod,
  nonCallableOnTypeIsIgnored,
} from 'reflection';

export {
  eventsAcrossImplementations,
  methodsAcrossImplementations,
  theGlobal,
  abortSignalIsNotAnEventTarget,
  nodeEventsHelpers,
  structuredCloneDiverges,
  inspectHidesListeners,
} from 'interop';

export {
  subclassConstruction,
  subclassOverridesAMethod,
  reflectConstructUsesNewTarget,
  subclassWithItsOwnState,
} from 'subclassing';

export {
  patchedBuiltinsAreNotUsed,
  omittedOptionsReadNothing,
  inheritedOptionMembersAreRead,
  inheritedHandleEventIsCalled,
} from 'pollution';

export {
  signalDoesNotRetainDeadTargets,
  targetRetainsItsListeners,
  endedRegistrationsReleaseTheirListeners,
} from 'gc';
