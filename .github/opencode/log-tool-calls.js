// opencode plugin for CI: logs each tool call as it starts and finishes. `opencode run` prints a
// tool call only once it completes, so a command that hangs would otherwise never appear.
import { writeSync } from 'node:fs';

const MAX_ARGS_LENGTH = 1000;
const startTimes = new Map();

function log(line) {
  writeSync(2, `[${new Date().toISOString()}] ${line}\n`);
}

// opencode loads a plugin given by path only if it default-exports an id and a server() function.
export default {
  id: 'log-tool-calls',
  server: async () => {
    log('log-tool-calls plugin loaded');
    return {
      'tool.execute.before': async (input, output) => {
        startTimes.set(input.callID, Date.now());
        let args = JSON.stringify(output.args);
        if (args.length > MAX_ARGS_LENGTH) {
          args = `${args.slice(0, MAX_ARGS_LENGTH)}...`;
        }
        log(`tool start: ${input.tool} ${args}`);
      },
      'tool.execute.after': async (input) => {
        const elapsed = (Date.now() - startTimes.get(input.callID)) / 1000;
        startTimes.delete(input.callID);
        log(`tool done: ${input.tool} after ${elapsed.toFixed(1)}s`);
      },
    };
  },
};
