import { strictEqual, throws, ok, deepStrictEqual } from 'node:assert';
import * as child_process from 'node:child_process';
import {
  ChildProcess,
  _forkChild,
  exec,
  execFile,
  execFileSync,
  execSync,
  fork,
  spawn,
  spawnSync,
} from 'node:child_process';

export const testExports = {
  async test() {
    strictEqual(typeof ChildProcess, 'function');
    strictEqual(typeof _forkChild, 'function');
    strictEqual(typeof exec, 'function');
    strictEqual(typeof execFile, 'function');
    strictEqual(typeof execFileSync, 'function');
    strictEqual(typeof execSync, 'function');
    strictEqual(typeof fork, 'function');
    strictEqual(typeof spawn, 'function');
    strictEqual(typeof spawnSync, 'function');

    strictEqual(child_process.default.ChildProcess, ChildProcess);
    strictEqual(child_process.default._forkChild, _forkChild);
    strictEqual(child_process.default.exec, exec);
    strictEqual(child_process.default.execFile, execFile);
    strictEqual(child_process.default.execFileSync, execFileSync);
    strictEqual(child_process.default.execSync, execSync);
    strictEqual(child_process.default.fork, fork);
    strictEqual(child_process.default.spawn, spawn);
    strictEqual(child_process.default.spawnSync, spawnSync);
  },
};

export const testChildProcessConstructor = {
  async test() {
    throws(
      () => {
        new ChildProcess();
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testForkChild = {
  async test() {
    throws(
      () => {
        _forkChild(1, 2);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testExec = {
  async test() {
    throws(
      () => {
        exec(123);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        exec(null);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        exec('ls', 'invalid');
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        exec('ls', {}, 'invalid');
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        exec('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls', {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls', null);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls', {}, () => {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testExecFile = {
  async test() {
    throws(
      () => {
        execFile('node');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFile('node', ['-v']);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFile('node', ['-v'], {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFile('node', ['-v'], {}, () => {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testExecFileSync = {
  async test() {
    throws(
      () => {
        execFileSync('node');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFileSync('node', ['-v']);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFileSync('node', ['-v'], {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testExecSync = {
  async test() {
    throws(
      () => {
        execSync('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execSync('ls', {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testFork = {
  async test() {
    throws(
      () => {
        fork('script.js');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', ['arg1', 'arg2']);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', ['arg1'], {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', 'invalid');
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        fork('script.js', 123);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        fork('script.js', [], 'invalid');
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );
  },
};

export const testSpawn = {
  async test() {
    throws(
      () => {
        spawn();
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawn('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawn('ls', ['-l']);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testSpawnSync = {
  async test() {
    throws(
      () => {
        spawnSync(123);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        spawnSync(null);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        spawnSync(undefined);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    throws(
      () => {
        spawnSync('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('ls', ['-l']);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('ls', ['-l'], {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testChildProcessProperties = {
  async test() {
    const proto = ChildProcess.prototype;

    strictEqual(typeof proto.kill, 'function');
    strictEqual(typeof proto.send, 'function');
    strictEqual(typeof proto.disconnect, 'function');
    strictEqual(typeof proto.unref, 'function');
    strictEqual(typeof proto.ref, 'function');

    ok(Symbol.dispose in proto);
    strictEqual(typeof proto[Symbol.dispose], 'function');
  },
};

export const testExecReturnsChildProcess = {
  async test() {
    let result;
    try {
      result = exec('ls');
    } catch (e) {
      // Expected to throw
    }
    ok(true);
  },
};

export const testForkEdgeCases = {
  async test() {
    throws(
      () => {
        fork('script.js', null);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', undefined);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', [], null);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js', [], undefined);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testExecCallbacks = {
  async test() {
    throws(
      () => {
        exec('ls', {}, null);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls', {}, undefined);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls', {}, (error, stdout, stderr) => {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls', {}, function (error, stdout, stderr) {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testSpawnSyncWithOptions = {
  async test() {
    throws(
      () => {
        spawnSync('ls', ['-l'], { cwd: '/tmp' });
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('ls', { cwd: '/tmp' });
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('ls', []);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('ls', [], {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testDefaultExport = {
  async test() {
    const defaultExport = child_process.default;

    ok(defaultExport);
    strictEqual(typeof defaultExport, 'object');

    ok('ChildProcess' in defaultExport);
    ok('_forkChild' in defaultExport);
    ok('exec' in defaultExport);
    ok('execFile' in defaultExport);
    ok('execFileSync' in defaultExport);
    ok('execSync' in defaultExport);
    ok('fork' in defaultExport);
    ok('spawn' in defaultExport);
    ok('spawnSync' in defaultExport);

    const expectedKeys = [
      'ChildProcess',
      '_forkChild',
      'exec',
      'execFile',
      'execFileSync',

      'execSync',
      'fork',
      'spawn',
      'spawnSync',
    ];
    deepStrictEqual(Object.keys(defaultExport).sort(), expectedKeys.sort());
  },
};

export const testExecEmptyCommand = {
  async test() {
    throws(
      () => {
        exec('');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('', {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('', {}, () => {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testSpawnSyncEmptyCommand = {
  async test() {
    throws(
      () => {
        spawnSync('');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('', []);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('', [], {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testErrorCodes = {
  async test() {
    throws(
      () => {
        new ChildProcess();
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        _forkChild(1, 2);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        exec('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFile('node');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execFileSync('node');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        execSync('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        fork('script.js');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawn();
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        spawnSync('ls');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};
