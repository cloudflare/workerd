// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone() of instances of classes implemented in JavaScript (jsg::Serializer's
// SERIALIZATION_TAG_JS_SERIALIZABLE), using the internal-testing cloneable classes that
// expose_serialization_test_class exposes. Reading each copy's label after cloning measures how
// fast the deserialized instances are to use.

/* global CloneableTestBox, CloneableTestError */

const boxes = Array.from(
  { length: 1000 },
  (_, i) => new CloneableTestBox(`box ${i}`, i)
);
const errors = Array.from(
  { length: 100 },
  (_, i) => new CloneableTestError(`error ${i}`)
);

function readLabels(copies) {
  let length = 0;
  for (const box of copies) length += box.label.length;
  return length;
}

export default {
  bench(b) {
    b.run('one box', () => b.blackBox(structuredClone(boxes[0])));
    b.run('array of 1000 boxes', () => b.blackBox(structuredClone(boxes)));
    b.run('array of 1000 boxes, then each label', () =>
      b.blackBox(readLabels(structuredClone(boxes)))
    );
    b.run('one error', () => b.blackBox(structuredClone(errors[0])));
    b.run('array of 100 errors', () => b.blackBox(structuredClone(errors)));
  },
};
