import repl from 'node:repl';
import assert from 'node:assert';
import { EventEmitter } from 'node:events';

export const replWriter = {
  test() {
    assert.throws(() => repl.writer(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /writer/,
    });
    assert.strictEqual(typeof repl.writer, 'function');
  },
};

export const replStart = {
  test() {
    assert.throws(() => repl.start(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /start/,
    });
    assert.strictEqual(typeof repl.start, 'function');
  },
};

export const replRecoverable = {
  test() {
    const error = new Error('test error');
    const recoverable = new repl.Recoverable(error);

    assert.strictEqual(recoverable instanceof SyntaxError, true);
    assert.strictEqual(recoverable instanceof repl.Recoverable, true);
    assert.strictEqual(recoverable.err, error);
    assert.strictEqual(recoverable.err.message, 'test error');

    const anotherError = new TypeError('type error');
    const anotherRecoverable = new repl.Recoverable(anotherError);
    assert.strictEqual(anotherRecoverable.err, anotherError);
  },
};

export const replREPLServer = {
  test() {
    assert.throws(() => new repl.REPLServer(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Interface/,
    });
    assert.strictEqual(typeof repl.REPLServer, 'function');
  },
};

export const replREPLServerMethods = {
  test() {
    const REPLServerProto = repl.REPLServer.prototype;

    assert.strictEqual(typeof REPLServerProto.setupHistory, 'function');
    assert.strictEqual(typeof REPLServerProto.defineCommand, 'function');
    assert.strictEqual(typeof REPLServerProto.displayPrompt, 'function');
    assert.strictEqual(typeof REPLServerProto.clearBufferedCommand, 'function');

    assert.strictEqual(typeof REPLServerProto.on, 'function');
    assert.strictEqual(typeof REPLServerProto.emit, 'function');
    assert.strictEqual(typeof REPLServerProto.once, 'function');
    assert.strictEqual(typeof REPLServerProto.removeListener, 'function');
  },
};

export const replREPLServerInheritance = {
  test() {
    assert.strictEqual(repl.REPLServer.prototype instanceof EventEmitter, true);
  },
};

export const replModes = {
  test() {
    assert.strictEqual(typeof repl.REPL_MODE_SLOPPY, 'symbol');
    assert.strictEqual(typeof repl.REPL_MODE_STRICT, 'symbol');
    assert.notStrictEqual(repl.REPL_MODE_SLOPPY, repl.REPL_MODE_STRICT);

    assert.strictEqual(repl.REPL_MODE_SLOPPY.toString(), 'Symbol(repl-sloppy)');
    assert.strictEqual(repl.REPL_MODE_STRICT.toString(), 'Symbol(repl-strict)');
  },
};

export const replBuiltinModules = {
  test() {
    assert.strictEqual(Array.isArray(repl.builtinModules), true);
    assert.strictEqual(repl.builtinModules.length > 0, true);

    assert.strictEqual(repl.builtinModules.includes('fs'), true);
    assert.strictEqual(repl.builtinModules.includes('path'), true);
    assert.strictEqual(repl.builtinModules.includes('http'), true);

    for (const module of repl.builtinModules) {
      assert.strictEqual(typeof module, 'string');
      assert.strictEqual(module[0] !== '_', true);
    }
  },
};

export const replBuiltinLibs = {
  test() {
    assert.strictEqual(Array.isArray(repl._builtinLibs), true);
    assert.deepStrictEqual(repl._builtinLibs, repl.builtinModules);
    assert.strictEqual(repl._builtinLibs, repl.builtinModules);
  },
};

export const replDefaultExport = {
  test() {
    assert.strictEqual(typeof repl, 'object');
    assert.strictEqual(typeof repl.writer, 'function');
    assert.strictEqual(typeof repl.start, 'function');
    assert.strictEqual(typeof repl.Recoverable, 'function');
    assert.strictEqual(typeof repl.REPLServer, 'function');
    assert.strictEqual(Array.isArray(repl.builtinModules), true);
    assert.strictEqual(Array.isArray(repl._builtinLibs), true);
    assert.strictEqual(typeof repl.REPL_MODE_SLOPPY, 'symbol');
    assert.strictEqual(typeof repl.REPL_MODE_STRICT, 'symbol');
  },
};

export const replModuleExports = {
  async test() {
    const replModule = await import('node:repl');

    assert.strictEqual(typeof replModule.default, 'object');
    assert.strictEqual(typeof replModule.writer, 'function');
    assert.strictEqual(typeof replModule.start, 'function');
    assert.strictEqual(typeof replModule.Recoverable, 'function');
    assert.strictEqual(typeof replModule.REPLServer, 'function');
    assert.strictEqual(Array.isArray(replModule.builtinModules), true);
    assert.strictEqual(Array.isArray(replModule._builtinLibs), true);
    assert.strictEqual(typeof replModule.REPL_MODE_SLOPPY, 'symbol');
    assert.strictEqual(typeof replModule.REPL_MODE_STRICT, 'symbol');

    assert.strictEqual(replModule.default.writer, replModule.writer);
    assert.strictEqual(replModule.default.start, replModule.start);
    assert.strictEqual(replModule.default.Recoverable, replModule.Recoverable);
    assert.strictEqual(replModule.default.REPLServer, replModule.REPLServer);
    assert.strictEqual(
      replModule.default.builtinModules,
      replModule.builtinModules
    );
    assert.strictEqual(
      replModule.default._builtinLibs,
      replModule._builtinLibs
    );
    assert.strictEqual(
      replModule.default.REPL_MODE_SLOPPY,
      replModule.REPL_MODE_SLOPPY
    );
    assert.strictEqual(
      replModule.default.REPL_MODE_STRICT,
      replModule.REPL_MODE_STRICT
    );
  },
};

export const replRecoverableError = {
  test() {
    const errors = [
      new Error('standard error'),
      new TypeError('type error'),

      new RangeError('range error'),
      new SyntaxError('syntax error'),
    ];

    for (const error of errors) {
      const recoverable = new repl.Recoverable(error);
      assert.strictEqual(recoverable.err, error);
      assert.strictEqual(recoverable instanceof SyntaxError, true);
      assert.strictEqual(recoverable instanceof Error, true);
    }
  },
};

export const replRecoverableConstructor = {
  test() {
    assert.strictEqual(typeof repl.Recoverable, 'function');
    assert.strictEqual(repl.Recoverable.name, 'Recoverable');

    const recoverable = new repl.Recoverable(new Error());
    assert.strictEqual(recoverable.constructor, repl.Recoverable);
    assert.strictEqual(recoverable.constructor.name, 'Recoverable');
  },
};

export const replBuiltinModulesFiltered = {
  test() {
    for (const module of repl.builtinModules) {
      assert.strictEqual(module.startsWith('_'), false);
    }

    for (const module of repl._builtinLibs) {
      assert.strictEqual(module.startsWith('_'), false);
    }
  },
};
