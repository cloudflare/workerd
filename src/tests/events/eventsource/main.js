// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the EventSource suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test. The default export is the
// server that connected EventSources fetch from, through each cell's
// `subrequest` binding.

export { default } from 'server';

export {
  staticMembers,
  prototypeMembers,
  prototypeKeys,
  prototypeChains,
  constructorAttributes,
  runtimeClassMarks,
  toStringTagBranding,
  disposeMember,
  instancesHaveNoOwnProperties,
  globalProperty,
} from 'api-surface';

export { inspectInstance, inspectPrototype } from 'inspect';

export {
  constructorRequiresNew,
  urlIsConvertedToAString,
  urlMustBeAbsolute,
  initMustBeAnObject,
  withCredentialsMustBeFalsy,
  fetcherMustBeAFetcher,
  conversionPrecedesChecks,
  newEventSourceState,
  onlyWithinARequest,
} from 'construction';

export { membersAreBrandChecked } from 'illegal-invocation';

export {
  fromRejectsNonStreams,
  fromRejectsLockedAndDisturbedStreams,
  fromNeedsNoReceiver,
  fromOpensSynchronously,
  fromReadsToTheEnd,
  fromChunkKinds,
  fromStringChunks,
  fromRejectsOtherChunks,
} from 'from';

export {
  dataLinesJoin,
  ignoredLines,
  leadingSpace,
  formatLeadingSpace,
  lineEndings,
  formatNewlines,
  formatFieldParsing,
  formatNullCharacter,
  unterminatedEventIsDropped,
  leadingBomIsStripped,
  formatBom2,
  invalidUtf8IsReplaced,
  utf8SplitAcrossChunks,
  namedEvents,
  lastEventIdPersistsAndResets,
  emptyEventField,
  bomSplitAcrossChunks,
  idWithNullIsIgnored,
  formatFieldData,
  crlfSplitAcrossChunks,
  errorFollowsTheLastMessage,
} from 'parser';

export { requestHeaders, failedFetchCloses } from 'request';

export {
  statusMustBeOk,
  contentTypeMustBeEventStream,
  redirectMovesTheUrl,
  responseWithoutABody,
} from 'response';

export {
  endOfStreamReconnects,
  lastEventIdIsSent,
  formatDataBeforeFinalEmptyLine,
  retrySetsTheReconnectionTime,
} from 'reconnection';

export { openEvent, messageEvent, errorEvent } from 'event-shapes';

export {
  handlersDefaultToNull,
  handlerPositionalOrdering,
  onPropertyForANamedEvent,
} from 'handlers';

export {
  closeIsIdempotentAndSilent,
  disposeCloses,
  closeFromAListener,
  closeCancelsAFromStream,
} from 'close';

export {
  throwingMessageListener,
  throwingOpenListener,
  throwingErrorListener,
} from 'listener-exceptions';

export {
  delayedNoBody,
  delayedBody,
  closeDuringReconnect,
  sameContextMessageDelivery,
} from 'context';

export {
  nativeStreamLifetime,
  queuedStreamLifetime,
  EventSourceGcDo,
  collectableInADurableObject,
} from 'gc';

export { fromStreamUnderPollution, connectionUnderPollution } from 'pollution';
