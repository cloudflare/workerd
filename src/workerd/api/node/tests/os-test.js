import os from 'node:os';
import { strictEqual, deepStrictEqual, throws } from 'node:assert';

export const osTest = {
  test() {
    strictEqual(os.EOL, '\n');
    strictEqual(os.devNull, '/dev/null');
    strictEqual(os.availableParallelism(), 1);
    strictEqual(os.arch(), 'x64');
    deepStrictEqual(os.cpus(), []);
    strictEqual(os.endianness(), 'LE');
    strictEqual(os.freemem(), 0);
    strictEqual(os.getPriority(), 0);
    strictEqual(os.homedir(), '/tmp/');
    strictEqual(os.hostname(), 'localhost');
    deepStrictEqual(os.loadavg(), [0, 0, 0]);
    strictEqual(os.machine(), 'x86_64');
    deepStrictEqual(os.networkInterfaces(), {});
    strictEqual(os.platform(), 'linux');
    strictEqual(os.release(), '');
    strictEqual(os.tmpdir(), '/tmp/');
    strictEqual(os.totalmem(), 0);
    strictEqual(os.type(), 'Linux');
    strictEqual(os.uptime(), 0);
    deepStrictEqual(os.userInfo(), {});
    strictEqual(os.version(), '');

    // setPriority and getPriority are non-ops
    os.setPriority(1, 2);
    strictEqual(os.getPriority(1), 0);
    throws(() => os.setPriority('a', 2), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(() => os.setPriority(1, 'a'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(() => os.getPriority('a'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(() => os.userInfo(1), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(() => os.userInfo({ encoding: 'bad encoding' }), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
    throws(() => os.userInfo({ encoding: 123 }), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
  },
};
