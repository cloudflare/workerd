import readline from 'node:readline';
import assert from 'node:assert';
import { EventEmitter } from 'node:events';
import promises from 'node:readline/promises';

export const readlineClearLine = {
  test() {
    assert.strictEqual(readline.clearLine(), false);
    assert.strictEqual(typeof readline.clearLine, 'function');
  },
};

export const readlineClearScreenDown = {
  test() {
    assert.strictEqual(readline.clearScreenDown(), false);
    assert.strictEqual(typeof readline.clearScreenDown, 'function');
  },
};

export const readlineCursorTo = {
  test() {
    assert.strictEqual(readline.cursorTo(), false);
    assert.strictEqual(typeof readline.cursorTo, 'function');
  },
};

export const readlineMoveCursor = {
  test() {
    assert.strictEqual(readline.moveCursor(), false);
    assert.strictEqual(typeof readline.moveCursor, 'function');
  },
};

export const readlineEmitKeypressEvents = {
  test() {
    assert.doesNotThrow(() => readline.emitKeypressEvents());
    assert.strictEqual(readline.emitKeypressEvents(), undefined);
    assert.strictEqual(typeof readline.emitKeypressEvents, 'function');
  },
};

export const readlineCreateInterface = {
  test() {
    // createInterface returns a no-op stub that matches unenv behavior
    const rl = readline.createInterface();
    assert.ok(rl instanceof readline.Interface);
    assert.strictEqual(rl.terminal, false);
    assert.strictEqual(rl.line, '');
    assert.strictEqual(rl.cursor, 0);
    assert.strictEqual(typeof readline.createInterface, 'function');
  },
};

export const readlineInterface = {
  test() {
    // Interface constructor returns a no-op stub that matches unenv behavior
    const rl = new readline.Interface();
    assert.ok(rl instanceof readline.Interface);
    assert.strictEqual(rl.terminal, false);
    assert.strictEqual(rl.line, '');
    assert.strictEqual(rl.cursor, 0);

    // Test methods return sensible defaults instead of throwing
    assert.strictEqual(rl.getPrompt(), '');
    rl.setPrompt('test'); // Should not throw
    rl.prompt(); // Should not throw
    rl.close(); // Should not throw
    rl.write('test'); // Should not throw
    assert.deepStrictEqual(rl.getCursorPos(), { rows: 0, cols: 0 });
    assert.strictEqual(rl.pause(), rl);
    assert.strictEqual(rl.resume(), rl);

    // question calls callback with empty string
    let questionAnswer = null;
    rl.question('test?', (answer) => {
      questionAnswer = answer;
    });
    assert.strictEqual(questionAnswer, '');

    // question with options also works
    let questionAnswer2 = null;
    rl.question('test?', {}, (answer) => {
      questionAnswer2 = answer;
    });
    assert.strictEqual(questionAnswer2, '');

    assert.strictEqual(typeof readline.Interface, 'function');
  },
};

export const readlineInterfaceMethods = {
  test() {
    const InterfaceProto = readline.Interface.prototype;

    assert.strictEqual(typeof InterfaceProto.getPrompt, 'function');
    assert.strictEqual(typeof InterfaceProto.setPrompt, 'function');
    assert.strictEqual(typeof InterfaceProto.prompt, 'function');
    assert.strictEqual(typeof InterfaceProto.question, 'function');
    assert.strictEqual(typeof InterfaceProto.pause, 'function');
    assert.strictEqual(typeof InterfaceProto.resume, 'function');
    assert.strictEqual(typeof InterfaceProto.close, 'function');
    assert.strictEqual(typeof InterfaceProto.write, 'function');
    assert.strictEqual(typeof InterfaceProto.getCursorPos, 'function');

    assert.strictEqual(typeof InterfaceProto.on, 'function');
    assert.strictEqual(typeof InterfaceProto.emit, 'function');
    assert.strictEqual(typeof InterfaceProto.once, 'function');
    assert.strictEqual(typeof InterfaceProto.removeListener, 'function');
  },
};

export const readlineInterfaceInheritance = {
  test() {
    assert.strictEqual(
      readline.Interface.prototype instanceof EventEmitter,
      true
    );
  },
};

export const readlinePromises = {
  test() {
    assert.strictEqual(typeof readline.promises, 'object');
    assert.strictEqual(typeof readline.promises.Interface, 'function');
    assert.strictEqual(typeof readline.promises.Readline, 'function');
    assert.strictEqual(typeof readline.promises.createInterface, 'function');
  },
};

export const readlinePromisesInterface = {
  async test() {
    // promises.Interface constructor returns a no-op stub that matches unenv behavior
    const rl = new readline.promises.Interface();
    assert.ok(rl instanceof readline.promises.Interface);
    assert.strictEqual(rl.terminal, false);
    assert.strictEqual(rl.line, '');
    assert.strictEqual(rl.cursor, 0);

    // Test methods return sensible defaults instead of throwing
    assert.strictEqual(rl.getPrompt(), '');
    rl.setPrompt('test'); // Should not throw
    rl.prompt(); // Should not throw
    rl.close(); // Should not throw
    rl.write('test'); // Should not throw
    assert.deepStrictEqual(rl.getCursorPos(), { rows: 0, cols: 0 });
    assert.strictEqual(rl.pause(), rl);
    assert.strictEqual(rl.resume(), rl);

    // question returns a promise that resolves to empty string
    const answer = await rl.question('test?');
    assert.strictEqual(answer, '');
  },
};

export const readlinePromisesCreateInterface = {
  test() {
    // promises.createInterface returns a no-op stub that matches unenv behavior
    const rl = readline.promises.createInterface();
    assert.ok(rl instanceof readline.promises.Interface);
    assert.strictEqual(rl.terminal, false);
    assert.strictEqual(rl.line, '');
    assert.strictEqual(rl.cursor, 0);
  },
};

export const readlinePromisesReadline = {
  async test() {
    // Readline constructor returns a no-op stub that matches unenv behavior
    const rl = new readline.promises.Readline(process.stdout);
    assert.ok(rl instanceof readline.promises.Readline);

    // Test methods return sensible defaults instead of throwing
    assert.strictEqual(rl.clearLine(0), rl);
    assert.strictEqual(rl.clearScreenDown(), rl);
    assert.strictEqual(rl.cursorTo(0), rl);
    assert.strictEqual(rl.cursorTo(0, 0), rl);
    assert.strictEqual(rl.moveCursor(1, 1), rl);
    assert.strictEqual(rl.rollback(), rl);

    // commit returns a promise that resolves to undefined
    const result = await rl.commit();
    assert.strictEqual(result, undefined);
  },
};

export const readlineDefaultExport = {
  test() {
    assert.strictEqual(typeof readline, 'object');
    assert.strictEqual(typeof readline.clearLine, 'function');
    assert.strictEqual(typeof readline.clearScreenDown, 'function');
    assert.strictEqual(typeof readline.createInterface, 'function');
    assert.strictEqual(typeof readline.cursorTo, 'function');
    assert.strictEqual(typeof readline.emitKeypressEvents, 'function');
    assert.strictEqual(typeof readline.moveCursor, 'function');
    assert.strictEqual(typeof readline.Interface, 'function');
    assert.strictEqual(typeof readline.promises, 'object');
  },
};

export const readlineModuleExports = {
  async test() {
    const readlineModule = await import('node:readline');

    assert.strictEqual(typeof readlineModule.default, 'object');
    assert.strictEqual(typeof readlineModule.clearLine, 'function');
    assert.strictEqual(typeof readlineModule.clearScreenDown, 'function');
    assert.strictEqual(typeof readlineModule.createInterface, 'function');
    assert.strictEqual(typeof readlineModule.cursorTo, 'function');
    assert.strictEqual(typeof readlineModule.emitKeypressEvents, 'function');
    assert.strictEqual(typeof readlineModule.moveCursor, 'function');
    assert.strictEqual(typeof readlineModule.Interface, 'function');
    assert.strictEqual(typeof readlineModule.promises, 'object');

    assert.strictEqual(
      readlineModule.default.clearLine,
      readlineModule.clearLine
    );
    assert.strictEqual(
      readlineModule.default.clearScreenDown,
      readlineModule.clearScreenDown
    );
    assert.strictEqual(
      readlineModule.default.createInterface,
      readlineModule.createInterface
    );
    assert.strictEqual(
      readlineModule.default.cursorTo,
      readlineModule.cursorTo
    );
    assert.strictEqual(
      readlineModule.default.emitKeypressEvents,
      readlineModule.emitKeypressEvents
    );
    assert.strictEqual(
      readlineModule.default.moveCursor,
      readlineModule.moveCursor
    );
    assert.strictEqual(
      readlineModule.default.Interface,
      readlineModule.Interface
    );
    assert.strictEqual(
      readlineModule.default.promises,
      readlineModule.promises
    );
  },
};

export const readlineFunctionReturnValues = {
  test() {
    const testArgs = [
      [],
      [null],
      [undefined],
      [1, 2],
      ['test', 123],
      [{}, [], () => {}],
    ];

    for (const args of testArgs) {
      assert.strictEqual(readline.clearLine(...args), false);
      assert.strictEqual(readline.clearScreenDown(...args), false);
      assert.strictEqual(readline.cursorTo(...args), false);
      assert.strictEqual(readline.moveCursor(...args), false);
      assert.strictEqual(readline.emitKeypressEvents(...args), undefined);
    }
  },
};

export const readlineImportedPromisesModule = {
  test() {
    assert.strictEqual(typeof promises, 'object');
    assert.strictEqual(typeof promises.createInterface, 'function');
  },
};
