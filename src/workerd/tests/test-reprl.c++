#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Libreprl is a .c file so the header needs to be in an 'extern "C"' block.
extern "C" {
#include "libreprl/libreprl.h"
}  // extern "C"

void print_splitter() {
  printf("---------------------------------\n");
}

struct reprl_context* ctx;

bool execute(const char* code) {
  uint64_t exec_time;
  const uint64_t SECONDS = 1000000;  // Timeout is in microseconds.
  print_splitter();
  printf("Executing: %s\n",code);
  int status = reprl_execute(ctx, code, strlen(code), 1 * SECONDS, &exec_time, 0);
  printf("Return code: %d\n",status);

  const char* fuzzout = reprl_fetch_fuzzout(ctx);
  printf("Fuzzout stdout:\n%s\n", fuzzout);
  fflush(stdout);

  const char* stdout_output = reprl_fetch_stdout(ctx);
  printf("Workerd stdout:\n%s\n", stdout_output);

  const char* stdout_err = reprl_fetch_stderr(ctx);
  printf("Workerd stderr:\n%s\n", stdout_err);

  if (RIFSIGNALED(status)) {
    printf("Process was terminated by signal %d\n", RTERMSIG(status));
  }
  print_splitter();
  fflush(stdout);
  fflush(stderr);

  // Check if the process exited successfully
  return RIFEXITED(status) && REXITSTATUS(status) == 0;
}

void expect_success(const char* code) {
  if (!execute(code)) {
    printf("Execution of \"%s\" failed\n", code);
    exit(1);
  }
}

void expect_failure(const char* code) {
  if (execute(code)) {
    printf("Execution of \"%s\" unexpectedly succeeded\n", code);
    exit(1);
  }
}

int main(int argc, char** argv) {
  ctx = reprl_create_context();

  const char* env[] = {"LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-16",nullptr};
  const char* workerd_path = argc > 1 ? argv[1] : nullptr;

  if(workerd_path == nullptr) {
    printf("Please specify the path to the workerd binary");
    return -1;
  }

  const char* args[] = {workerd_path, nullptr};
  if (reprl_initialize_context(ctx, args, env, 1, 1) != 0) {
    printf("REPRL initialization failed\n");
    return -1;
  }

  // Basic functionality test
  if (!execute("let greeting = \"Hello World!\";")) {
    printf("Script execution failed\n");
    return -1;
  }

  // Verify that runtime exceptions can be detected
  expect_failure("throw 'failure';");
  expect_success("42;");
  // Verify that existing state is properly reset between executions
  expect_success("globalProp = 42; Object.prototype.foo = \"bar\";");
  expect_success("if (typeof(globalProp) !== 'undefined') throw 'failure'");
  expect_success("if (typeof(({}).foo) !== 'undefined') throw 'failure'");

  // Verify that rejected promises are properly reset between executions
  expect_failure("function fail() { throw 42; }; fail()");
  // async is not failing in workerd
  //expect_failure("async function fail() { throw 42; }; fail()");
  fflush(stdout);
  puts("OK");
  return 0;
}
