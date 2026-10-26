// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the structuredClone suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time SyntaxError
// instead of a silently dropped test.

export {
  placement,
  functionShape,
  notAConstructor,
  sourceText,
  replaceableAndDeletable,
} from 'api-surface';

export {
  acceptedReceivers,
  rejectedReceivers,
  receiverCheckedBeforeArguments,
} from 'receiver';

export {
  valueIsRequired,
  extraArgumentsIgnored,
  absentOptions,
  objectOptions,
  primitiveOptionsRejected,
  transferMemberLookup,
  transferGetterThrows,
} from 'arguments';

export {
  arrayTransfers,
  nonObjectMemberRejected,
  nonIterableObjectRejected,
  iterablesOtherThanArrays,
  nonObjectElementsRejected,
  nonTransferableObjectsRejected,
  arrayIteratorUse,
  elementGetters,
} from 'transfer-conversion';

export {
  plainValues,
  hostObjects,
  uncloneableValues,
  valueGetterThrows,
  transferSemantics,
} from 'cloning';

export { patchedBuiltins, errorsUseTheRealTypeError } from 'pollution';
