// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the MessageChannel suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  prototypeMethods,
  onmessageAccessor,
  prototypeKeys,
  prototypeChains,
  constructorAttributes,
  runtimeClassMarks,
  toStringTagBranding,
  channelOwnProperties,
  globalsAndWorkerThreads,
} from 'api-surface';

export { inspectPort, inspectChannel, inspectPrototype } from 'inspect';

export {
  messagePortIsNotConstructible,
  constructorsRequireNew,
  messageChannelIsSubclassable,
} from 'construction';

export { membersAreBrandChecked } from 'illegal-invocation';

export {
  primitiveOptionsAreRejected,
  emptyTransferListsAreAccepted,
  transferMustBeAnArray,
  nonEmptyTransferListsAreUnsupported,
  conversionReadsBeforeRefusing,
  proxiedArrayOptions,
} from 'post-message-arguments';

export {
  sharedArrayBuffersAreRejected,
  uncloneableValuesThrow,
  closedPairStillSerializes,
  closingDuringSerializationDropsTheMessage,
  receiverGetsACopy,
  blobsRoundTrip,
} from 'cloning';

export {
  postedBeforeListening,
  postedWhileListening,
  deliveredEventShape,
  deliveryIsDeferred,
} from 'delivery';

export { userConstructedEventReflectsPorts } from 'message-event';

export {
  onmessageDefaultsToNull,
  onmessagePositionalOrdering,
  onmessageClearedTakesFreshPosition,
  onmessageNonCallableStartsPort,
  onmessagePrimitiveClears,
  onmessageHandlerReceiver,
} from 'onmessage';

export {
  addEventListenerStartsPort,
  startWithoutListenersConsumesTheQueue,
  removingLastListenerRequeues,
  onceListenerReturnsPortToPending,
  abortedListenerReturnsPortToPending,
  closedPortIsTerminal,
  postingToAClosedPairDrops,
} from 'port-queue';

export {
  closeFiresOnBothPortsPeerFirst,
  oncloseProperty,
  throwingCloseListenerEscapesClose,
} from 'close';

export {
  throwingMessageListener,
  throwingMessageErrorListener,
} from 'listener-exceptions';

export { queuedMessagesSurviveGc, portsSurviveGc } from 'gc';

export { deliveryIgnoresPollutedPrototypes } from 'pollution';
