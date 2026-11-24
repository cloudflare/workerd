// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the Blob suite. Re-exports are explicit (not `export *`)
// so a name collision between modules is a load-time SyntaxError instead of
// a silently dropped test.

import { WorkerEntrypoint } from 'cloudflare:workers';

export {
  globalPlacement,
  constructorShape,
  prototypeShape,
  instanceShape,
  toStringTag,
  brandChecks,
  subclassing,
} from 'api-surface';

export {
  constructorRequiresNew,
  parts,
  sourcesAreCopied,
  detachedSources,
  conversionOrder,
  options,
  endings,
  typeNormalization,
  loneSurrogates,
  fileConstructorIgnoresItsPrototype,
  sliceIgnoresAReplacedPrototype,
  files,
  sizeLimit,
  resizableBuffers,
} from 'construction';

export {
  sliceRanges,
  sliceType,
  sliceIsANewBlob,
  sliceConversionOrder,
} from 'slicing';

export {
  formDataKeepsItsFilesAlive,
  unreachableFormDataIsCollected,
  formDataFileCyclesAreCollected,
} from 'gc';

export { text, copies, stream, pollution } from 'reading';

export {
  structuredCloneRoundTrip,
  filesDoNotSerialize,
  subclassInstancesCloneAsBlob,
  posted,
  overRpc,
} from 'serialization';

export {
  responseBlob,
  blobBodies,
  formDataEntries,
  formDataRoundTrip,
} from 'consumers';

export { inspectInstances } from 'inspect';

// The RPC binding's entrypoint (serialization.js).
export class BlobRpc extends WorkerEntrypoint {
  echo(blob) {
    return blob;
  }

  makeBlob() {
    return new Blob(['xy'], { type: 'a/b' });
  }

  async describe(blob) {
    return [
      blob.constructor.name,
      blob.size,
      blob.type,
      await blob.text(),
    ].join();
  }
}
