// Golden file test - just outputs to stdio for verification
// This test produces predictable output that can be verified against golden files

export const stdioOutputTest = {
  test() {
    // Console output
    console.log('Console log test message');
    console.log('Multiple', 'arguments', 'test');
    console.log({ object: 'test', number: 42 });

    console.error('Console error test message');
    console.error('Error with', 'multiple', 'arguments');
    console.error({ error: 'test', code: 500 });

    // Process stdout/stderr output
    process.stdout.write('STDOUT: Message 1');
    process.stderr.write('STDERR: Error 1');
    console.log('CONSOLE.LOG: Message 2');
    console.error('CONSOLE.ERROR: Error 2');
    process.stdout.write('STDOUT: Message 3');
    process.stderr.write('STDERR: Error 3');

    // Direct writes
    process.stdout.write('Hello stdout');
    process.stdout.write('Line 1');
    process.stdout.write('Line 2');

    process.stderr.write('Hello stderr');
    process.stderr.write('Error: Test error message');
    process.stderr.write('Warning: Test warning message\n'); // Newline to separate from debug output
  },
};
