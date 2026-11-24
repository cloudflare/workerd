// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the Cloudflare global's suite. Re-exports are explicit
// (not `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  globalPlacement,
  sameObjectEveryTime,
  replaceable,
  constructorShape,
  illegalConstructor,
  illegalConstructorStack,
  illegalConstructorAmongReplacedBuiltIns,
  prototypeShape,
  instanceShape,
  toStringTag,
  sourceText,
  prototypeProperty,
  inherited,
  readOnly,
  inspected,
  json,
  notSerializable,
} from 'shape';

export {
  flagsShape,
  flagsListEveryEnableFlagInSchemaOrder,
  flagsAreTheSameObject,
  flagsCannotChange,
  flagValues,
  flagsReadAsAnyObject,
  flagsClone,
} from 'flags';
