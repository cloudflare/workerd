// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { test } from 'node:test';
import ts from 'typescript';
import { printer } from '../../src/print';
import { createCommentsTransformer } from '../../src/transforms';

test('createCommentsTransformer: adds comments by name', () => {
  const source = ts.createSourceFile(
    'source.ts',
    `declare class Thing {
    get size(): number;
    toString(): string;
    valueOf(): number;
}
declare function thing(): Thing;
`,
    ts.ScriptTarget.ESNext,
    /* setParentNodes */ true,
    ts.ScriptKind.TS
  );
  const result = ts.transform(source, [
    createCommentsTransformer({
      Thing: { $: '* A thing. ', size: '* Its size. ' },
      thing: { $: '* Makes a thing. ' },
    }),
  ]);
  // toString() and valueOf() have no comments, whatever the comments data's
  // prototype has.
  assert.strictEqual(
    printer.printFile(result.transformed[0]),
    `/** A thing. */
declare class Thing {
    /** Its size. */
    get size(): number;
    toString(): string;
    valueOf(): number;
}
/** Makes a thing. */
declare function thing(): Thing;
`
  );
});

test('createCommentsTransformer: documents an interface, not also its constructor variable', () => {
  const source = ts.createSourceFile(
    'source.ts',
    `interface Thing {
    readonly size: number;
}
declare var Thing: {
    prototype: Thing;
    new (): Thing;
};
declare var thing: Thing;
`,
    ts.ScriptTarget.ESNext,
    /* setParentNodes */ true,
    ts.ScriptKind.TS
  );
  const result = ts.transform(source, [
    createCommentsTransformer({
      Thing: { $: '* A thing. ', size: '* Its size. ' },
      thing: { $: '* The thing. ' },
    }),
  ]);
  // A variable no interface shares a name with keeps its comment.
  assert.strictEqual(
    printer.printFile(result.transformed[0]),
    `/** A thing. */
interface Thing {
    /** Its size. */
    readonly size: number;
}
declare var Thing: {
    prototype: Thing;
    new (): Thing;
};
/** The thing. */
declare var thing: Thing;
`
  );
});
