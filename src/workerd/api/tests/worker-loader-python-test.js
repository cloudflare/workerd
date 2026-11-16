// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Worker Loader tests of dynamic Workers written in Python.
import assert from 'node:assert';

export let pythonBasics = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('pythonBasics', () => {
      return {
        compatibilityDate: '2025-01-01',
        mainModule: 'foo.py',
        compatibilityFlags: ['python_workers', 'python_no_global_handlers'],
        modules: {
          'foo.py': `
from workers import WorkerEntrypoint
class Default(WorkerEntrypoint):
  async def greet(self, name):
    return "Hello, " + name
class alternate(WorkerEntrypoint):
  async def greet(self, name):
    return "Welcome, " + name
          `,
        },
      };
    });

    {
      let result = await worker.getEntrypoint().greet('Stacey');
      assert.strictEqual(result, 'Hello, Stacey');
    }

    {
      let result = await worker.getEntrypoint('alternate').greet('Anthony');
      assert.strictEqual(result, 'Welcome, Anthony');
    }
  },
};

export let pythonArbitraryModuleExtensions = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('pythonArbitraryModuleExtensions', () => {
      return {
        compatibilityDate: '2026-05-15',
        mainModule: 'foo.py',
        compatibilityFlags: ['python_workers'],
        modules: {
          'foo.py': `
from workers import WorkerEntrypoint
from pathlib import Path
import submodule;
from subdirectory import subsubmodule;
class Default(WorkerEntrypoint):
  async def greet(self, name):
    with open(str(Path(__file__).parent) + "/python_modules/data.txt", 'r') as f:
      return f.read() + name
  async def bytes(self):
    with open(str(Path(__file__).parent) + "/python_modules/data.mycoolextension", 'rb') as f:
      return (f.read())
  async def modules_in_main_directory(self):
    return submodule.submodule_value + subsubmodule.subsubmodule_value;
          `,
          'submodule.py': `submodule_value = 10`,
          'subdirectory/subsubmodule.py': `subsubmodule_value = 20`,
          'python_modules/data.txt': 'Hello, ',
          'python_modules/data.mycoolextension': {
            data: new Uint8Array([1, 2, 3, 4]),
          },
        },
      };
    });

    let result = await worker.getEntrypoint().greet('Stacey');
    assert.strictEqual(result, 'Hello, Stacey');

    assert.deepEqual(
      await worker.getEntrypoint().bytes(),
      new Uint8Array([1, 2, 3, 4])
    );
    assert.strictEqual(
      await worker.getEntrypoint().modules_in_main_directory(),
      30
    );
  },
};

// A dynamic Worker whose main module is Python gets `python_workers` enabled
// automatically, so the caller never has to name it. That injection has to
// happen *before* the compatibility flags are compiled because `python_workers`
// gates a chain of `impliedByAfterDate` flags.
export let pythonImplicitFlagsRespectCompatDate = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('pythonImplicitFlagsRespectCompatDate', () => {
      return {
        compatibilityDate: '2026-05-15',
        mainModule: 'foo.py',
        modules: {
          'foo.py': `
import sys
from workers import WorkerEntrypoint
class Default(WorkerEntrypoint):
  async def python_version(self):
    return "%d.%d" % sys.version_info[:2]
          `,
        },
      };
    });

    // The Pyodide release gated on bare `python_workers` ships Python 3.12; the one implied by
    // this compat date (via `python_workers_20250116`) ships 3.13.
    assert.strictEqual(await worker.getEntrypoint().python_version(), '3.13');
  },
};

export let minimalPythonPackage = {
  async test(ctrl, env, ctx) {
    let worker = env.loader.get('minimalPythonPackage', () => {
      return {
        compatibilityDate: '2026-05-15',
        mainModule: 'main.py',
        compatibilityFlags: ['python_workers'],
        modules: {
          'main.py': `
from workers import WorkerEntrypoint
class Default(WorkerEntrypoint):
  async def main(self):
    import testpkg
    return testpkg.testfunc()
`,
          'python_modules/testpkg/pkgmodule.py': `
def testfunc():
  return "Hello dynamic worker package"
`,
          'python_modules/testpkg/__init__.py': `
from . import pkgmodule
testfunc = pkgmodule.testfunc
`,
        },
      };
    });

    {
      let result = await worker.getEntrypoint().main();
      assert.strictEqual(result, 'Hello dynamic worker package');
    }
  },
};
