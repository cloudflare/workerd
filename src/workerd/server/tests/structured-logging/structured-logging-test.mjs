import { spawn } from 'node:child_process';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WD_TEST_CONFIG !== undefined,
  'You must set the WD_TEST_CONFIG environment variable.'
);

test('structured logging produces valid JSON with required fields', async () => {
  const result = await new Promise((resolve) => {
    let output = '';
    const child = spawn(env.WORKERD_BINARY, ['test', env.WD_TEST_CONFIG], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    child.stdout.on('data', (data) => (output += data));
    child.stderr.on('data', (data) => (output += data));
    child.on('close', () => resolve(output));
  });

  // Parse as JSON - extract the first JSON line
  let logEntry;
  assert.doesNotThrow(() => {
    // Split by lines and find the first valid JSON line
    const lines = result.split('\n');
    const jsonLine = lines.find((line) => line.trim().startsWith('{'));
    assert(jsonLine, 'No JSON line found in output');
    logEntry = JSON.parse(jsonLine);
  }, `Output is not valid JSON: ${result}`);

  // Verify required structure
  assert.strictEqual(
    typeof logEntry,
    'object',
    'Log entry should be an object'
  );
  assert(logEntry !== null, 'Log entry should not be null');

  // Verify required fields exist with correct types
  assert(
    typeof logEntry.level === 'string',
    `level should be string, got: ${typeof logEntry.level}`
  );
  assert(
    typeof logEntry.message === 'string',
    `message should be string, got: ${typeof logEntry.message}`
  );
  assert(
    typeof logEntry.timestamp === 'string',
    `timestamp should be string, got: ${typeof logEntry.timestamp}`
  );

  // Verify timestamp is a valid ISO date
  assert.doesNotThrow(() => {
    new Date(logEntry.timestamp);
  }, `timestamp should be valid date: ${logEntry.timestamp}`);

  // Verify error content is present in the message
  assert(
    logEntry.message.includes('SyntaxError'),
    `Missing SyntaxError in message: ${logEntry.message}`
  );
  assert(
    logEntry.message.includes('javascript'),
    `Missing javascript reference in message: ${logEntry.message}`
  );
});
