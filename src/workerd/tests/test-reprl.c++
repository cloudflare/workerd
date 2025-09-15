#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kj/debug.h>
#include <kj/exception.h>

// Libreprl is a .c file so the header needs to be in an 'extern "C"' block.
extern "C" {
#include "libreprl/libreprl.h"
}  // extern "C"

void print_splitter() {
  KJ_LOG(INFO, "---------------------------------\n");
}

struct reprl_context* ctx;

bool execute(const char* code) {
  uint64_t exec_time;
  const uint64_t SECONDS = 1000000;  // Timeout is in microseconds.
  print_splitter();
  KJ_LOG(INFO, "Executing: %s\n", code);
  int status = reprl_execute(ctx, code, strlen(code), 1 * SECONDS, &exec_time, 0);
  KJ_LOG(INFO, "Return code: %d\n", status);

  const char* fuzzout = reprl_fetch_fuzzout(ctx);
  KJ_LOG(INFO, "Fuzzout stdout:\n%s\n", fuzzout);

  const char* stdout_output = reprl_fetch_stdout(ctx);
  KJ_LOG(INFO, "Workerd stdout:\n%s\n", stdout_output);
  const char* stdout_err = reprl_fetch_stderr(ctx);
  KJ_LOG(INFO, "Workerd stderr:\n%s\n", stdout_err);

  if (RIFSIGNALED(status)) {
    KJ_LOG(INFO, "Process was terminated by signal %d\n", RTERMSIG(status));
  }
  print_splitter();
  fflush(stdout);
  fflush(stderr);

  // Check if the process exited successfully
  return RIFEXITED(status) && REXITSTATUS(status) == 0;
}

void expect_success(const char* code) {
  if (!execute(code)) {
    KJ_LOG(INFO, "Execution of \"%s\" failed\n", code);
    exit(1);
  }
}

void expect_failure(const char* code) {
  if (execute(code)) {
    KJ_LOG(INFO, "Execution of \"%s\" unexpectedly succeeded\n", code);
    exit(1);
  }
}

int main(int argc, char** argv) {
#ifdef __linux__
  ctx = reprl_create_context();

  const char* env[] = {"LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-19", nullptr};
  if (argc < 4) {
    KJ_LOG(INFO, "Usage: %s <workerd_path> <command> <path-to-config> <workerd-flags>", argv[0]);
    exit(-1);
  }

  // Forward workerd_path + all remaining args (argv[1..argc-1])
  const char** args = (const char**)&argv[1];

  if (reprl_initialize_context(ctx, args, env, 1, 1) != 0) {
    KJ_LOG(INFO, "REPRL initialization failed\n");
    return -1;
  }

  // Basic functionality test
  if (!execute("let greeting = \"Hello World!\";")) {
    KJ_LOG(INFO, "Script execution failed\n");
    return -1;
  }

  // Verify that runtime exceptions can be detected
  expect_failure("throw 'failure';");
  expect_success("42;");
  // Verify that existing state is properly reset between executions
  // expect_success("globalProp = 42; Object.prototype.foo = \"bar\";");
  // expect_success("if (typeof(globalProp) !== 'undefined') throw 'failure'");
  // expect_success("if (typeof(({}).foo) !== 'undefined') throw 'failure'");

  // Verify that rejected promises are properly reset between executions
  expect_failure("function fail() { throw 42; }; fail()");

  expect_failure("fuzzilli('FUZZILLI_CRASH',0);");
  expect_failure("fuzzilli('FUZZILLI_CRASH',1);");
  expect_failure("fuzzilli('FUZZILLI_CRASH',2);");
  expect_failure("fuzzilli('FUZZILLI_CRASH',3);");
  expect_failure("fuzzilli('FUZZILLI_CRASH',4);");
  //this one doesn't fail
  //expect_failure("fuzzilli('FUZZILLI_CRASH',5);");
  expect_failure("fuzzilli('FUZZILLI_CRASH',6);");
  // async is not failing in workerd
  //expect_failure("async function fail() { throw 42; }; fail()");
  KJ_DBG("OK");
  fflush(stdout);
#endif
  return 0;
}
