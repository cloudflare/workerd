// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A FormData keeps its File entries alive, and they are collected with it,
// even when a File refers back to the FormData: FormData::visitForGc()
// traces the entries of either implementation.

import { strictEqual } from 'node:assert';

// Collects until `done()` holds, or a few times when there is no condition.
// FinalizationRegistry callbacks run in a later task.
async function collect(done = () => false) {
  for (let i = 0; i < 10 && !done(); i++) {
    gc();
    await scheduler.wait(1);
  }
}

export const formDataKeepsItsFilesAlive = {
  async test() {
    const form = (() => {
      const form = new FormData();
      form.append('file', new File(['xy'], 'n', { type: 'a/b' }));
      form.append('blob', new Blob(['zw']));
      form.append('slice', new Blob(['abcd']).slice(1, 3));
      return form;
    })();
    await collect();
    const file = form.get('file');
    strictEqual(file.name, 'n');
    strictEqual(file.type, 'a/b');
    strictEqual(await file.text(), 'xy');
    strictEqual(await form.get('blob').text(), 'zw');
    strictEqual(await form.get('slice').text(), 'bc');
    strictEqual(
      new TextDecoder()
        .decode(await new Response(form).arrayBuffer())
        .includes('xy'),
      true
    );
  },
};

export const unreachableFormDataIsCollected = {
  async test() {
    let collected = 0;
    const registry = new FinalizationRegistry(() => {
      collected++;
    });
    const kCount = 16;
    (() => {
      for (let i = 0; i < kCount; i++) {
        const form = new FormData();
        const file = new File([`${i}`], `${i}`);
        form.append('file', file);
        registry.register(form, i);
        registry.register(file, i);
      }
    })();
    await collect(() => collected === 2 * kCount);
    strictEqual(collected, 2 * kCount);
  },
};

export const formDataFileCyclesAreCollected = {
  async test() {
    let collected = 0;
    const registry = new FinalizationRegistry(() => {
      collected++;
    });
    const kCount = 16;
    (() => {
      for (let i = 0; i < kCount; i++) {
        const form = new FormData();
        const file = new File([`${i}`], `${i}`);
        form.append('file', file);
        // The entry is the File itself, which now refers to the FormData.
        file.form = form;
        registry.register(form, i);
        registry.register(file, i);
      }
    })();
    await collect(() => collected === 2 * kCount);
    strictEqual(collected, 2 * kCount);
  },
};
