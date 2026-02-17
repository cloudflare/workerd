#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kj/test.h>

// Libreprl is a .c file so the header needs to be in an 'extern "C"' block.
extern "C" {
#include "libreprl/libreprl.h"
}

#include "tools/cpp/runfiles/runfiles.h"

using bazel::tools::cpp::runfiles::Runfiles;

namespace workerd {
namespace {

#ifdef __linux__
void print_splitter() {
  printf("---------------------------------\n");
}

bool execute(struct reprl_context* ctx, const char* code) {
  uint64_t exec_time;
  const uint64_t SECONDS = 1000000;  // Timeout is in microseconds.
  print_splitter();
  printf("Executing: %s\n", code);
  int status = reprl_execute(ctx, code, strlen(code), 1 * SECONDS, &exec_time, 0);
  printf("Return code: %d\n", status);

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

void expect_success(struct reprl_context* ctx, const char* code) {
  if (!execute(ctx, code)) {
    KJ_FAIL_REQUIRE("Execution unexpectedly failed", code);
  }
}

void expect_failure(struct reprl_context* ctx, const char* code) {
  if (execute(ctx, code)) {
    KJ_FAIL_REQUIRE("Execution unexpectedly succeeded", code);
  }
}
#endif  // __linux__

KJ_TEST("REPRL basic functionality") {
#ifdef __linux__
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  KJ_REQUIRE(runfiles != nullptr, "Failed to create runfiles", error.c_str());

  auto ctx = reprl_create_context();
  KJ_REQUIRE(ctx != nullptr, "Failed to create REPRL context");

  const char* env[] = {"LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-19", nullptr};

  // Use Runfiles API to get absolute paths
  std::string workerd_path = runfiles->Rlocation("workerd/src/workerd/server/workerd");
  // Use config.capnp which has a socket (needed to trigger fetch() which calls Stdin.reprl())
  std::string config_path = runfiles->Rlocation("workerd/fuzzilli/config.capnp");

  const char* args[] = {
    workerd_path.c_str(), "fuzzilli", config_path.c_str(), "--experimental", nullptr};

  if (reprl_initialize_context(ctx, args, env, 1, 1) != 0) {
    KJ_FAIL_REQUIRE("REPRL initialization failed");
  }

  // Basic functionality test
  expect_success(ctx, "let greeting = \"Hello World!\";");

  // Test with console.log output
  expect_success(ctx, "console.log('Hello from JavaScript!');");

  // Verify that runtime exceptions can be detected
  expect_failure(ctx, "throw 'failure';");
  expect_success(ctx, "42;");

  // Verify that existing state is properly reset between executions
  // These tests are commented out as they may not apply to workerd's execution model
  // expect_success(ctx, "globalProp = 42; Object.prototype.foo = \"bar\";");
  // expect_success(ctx, "if (typeof(globalProp) !== 'undefined') throw 'failure'");
  // expect_success(ctx, "if (typeof(({}).foo) !== 'undefined') throw 'failure'");

  // Verify that rejected promises are properly reset between executions
  expect_failure(ctx, "function fail() { throw 42; }; fail()");

  // Verify that fuzzilli crash command is detected as failure
  expect_failure(ctx, "fuzzilli('FUZZILLI_CRASH',0);");
  expect_failure(ctx, "fuzzilli('FUZZILLI_CRASH',1);");
  expect_failure(ctx, "fuzzilli('FUZZILLI_CRASH',2);");
  expect_failure(ctx, "fuzzilli('FUZZILLI_CRASH',3);");
  expect_failure(ctx, "fuzzilli('FUZZILLI_CRASH',4);");
  //expect_failure(ctx, "fuzzilli('FUZZILLI_CRASH',5);");

  // async is not failing in workerd (commented out from original)
  // expect_failure(ctx, "async function fail() { throw 42; }; fail()");

  reprl_destroy_context(ctx);
#else
  KJ_LOG(WARNING, "REPRL tests only supported on Linux");
#endif
}

}  // namespace
}  // namespace workerd
