// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import * as vm from 'node:vm';
import { setImmediate } from 'node:timers/promises';

async function collect() {
  // WeakRef keeps targets alive until the current job ends.
  for (let i = 0; i < 2; i++) {
    const collected = gc({
      type: 'major',
      execution: 'async',
      flavor: 'last-resort',
    });
    // Keep request I/O pending while V8 runs the stackless collection task.
    await setImmediate();
    await collected;
  }
}

export const releasedModuleCallbacks = {
  async test() {
    const context = vm.createContext();
    async function createGraph() {
      const marker = {};
      const synthetic = new vm.SyntheticModule(
        ['marker'],
        () => {
          synthetic.setExport('marker', marker);
        },
        { context }
      );
      const source = new vm.SourceTextModule(
        'import { marker } from "synthetic"; export { marker };',
        { context }
      );
      await source.link(() => synthetic);
      await source.evaluate();
      assert.equal(source.namespace.marker, marker);
      return new WeakRef(marker);
    }
    const marker = await createGraph();
    await collect();
    assert.equal(marker.deref(), undefined);
    assert.equal(vm.runInContext('1 + 1', context), 2);
  },
};

export const releasedMainContextCallbacks = {
  async test() {
    async function createModule() {
      const marker = {};
      const source = new vm.SourceTextModule(
        'export const value = import.meta.value;',
        {
          initializeImportMeta(meta) {
            meta.value = marker;
          },
        }
      );
      await source.link(() => assert.fail('unexpected dependency'));
      await source.evaluate();
      assert.equal(source.namespace.value, marker);
      return new WeakRef(marker);
    }
    const marker = await createModule();
    await collect();
    assert.equal(marker.deref(), undefined);
  },
};

export const liveFunctionRetainsImportCallback = {
  async test() {
    const dependency = new vm.SyntheticModule(['value'], () => {
      dependency.setExport('value', 42);
    });
    await dependency.evaluate();
    function compile() {
      const marker = {};
      const fn = vm.compileFunction('return import("dependency")', [], {
        importModuleDynamically() {
          assert.equal(typeof marker, 'object');
          return dependency;
        },
      });
      return { fn, marker: new WeakRef(marker) };
    }
    let { fn, marker } = compile();
    await collect();
    assert.equal(typeof marker.deref(), 'object');
    assert.equal((await fn()).value, 42);
    fn = undefined;
    await collect();
    assert.equal(marker.deref(), undefined);
  },
};
