// KJ_TEST version of REPRL tests for workerd's Fuzzilli integration.
//
// These tests verify that workerd's REPRL (Read-Eval-Print-Reset-Loop) protocol
// implementation works correctly for fuzzing with Fuzzilli.
//
// To run:
//   bazel test --config=fuzzilli //src/workerd/tests:test-reprl-kj --action_env=CC=/usr/bin/clang-19
//
// Or build and run directly:
//   bazel build --config=fuzzilli //src/workerd/tests:test-reprl-kj --action_env=CC=/usr/bin/clang-19
//   ./bazel-bin/src/workerd/tests/test-reprl-kj_binary

#include <kj/test.h>
#include <kj/debug.h>
#include <unistd.h>  // for access()
#include <cstdlib>   // for getenv()
#include <cstdio>    // for fprintf()
#include "tools/cpp/runfiles/runfiles.h"

// Libreprl is a .c file so the header needs to be in an 'extern "C"' block.
extern "C" {
#include "libreprl/libreprl.h"
}  // extern "C"

using bazel::tools::cpp::runfiles::Runfiles;

namespace workerd {
namespace {

void printSplitter() {
  KJ_LOG(INFO, "---------------------------------");
}

class ReprlContext {
 public:
  ReprlContext(): ctx(reprl_create_context()) {
    KJ_REQUIRE(ctx != nullptr, "Failed to create REPRL context");
  }

  ~ReprlContext() noexcept(false) {
    if (ctx) {
      reprl_destroy_context(ctx);
    }
  }

  void initialize(kj::ArrayPtr<const char* const> args, kj::ArrayPtr<const char* const> env) {
    // Cast away const for libreprl interface compatibility
    KJ_REQUIRE(reprl_initialize_context(ctx, const_cast<const char**>(args.begin()),
                   const_cast<const char**>(env.begin()), 1, 1) == 0,
        "REPRL initialization failed");
  }

  struct ExecutionResult {
    int status;
    uint64_t execTime;
    kj::String fuzzout;
    kj::String stdoutOutput;
    kj::String stderrOutput;

    bool wasSignaled() const {
      return RIFSIGNALED(status);
    }
    int termSignal() const {
      return RTERMSIG(status);
    }
    bool exitedSuccessfully() const {
      return RIFEXITED(status) && REXITSTATUS(status) == 0;
    }
  };

  ExecutionResult execute(kj::StringPtr code, uint64_t timeoutMicros = 5000000) {
    uint64_t execTime;

    printSplitter();
    KJ_LOG(INFO, "Executing:", code);
    fprintf(stderr, "About to call reprl_execute with %zu byte script, timeout=%lu us\n",
            code.size(), timeoutMicros);
    fflush(stderr);

    int status = reprl_execute(ctx, code.cStr(), code.size(), timeoutMicros, &execTime, 0);

    fprintf(stderr, "reprl_execute returned status=%d\n", status);
    fflush(stderr);
    KJ_LOG(INFO, "Return code:", status);

    ExecutionResult result = {.status = status,
      .execTime = execTime,
      .fuzzout = kj::str(reprl_fetch_fuzzout(ctx)),
      .stdoutOutput = kj::str(reprl_fetch_stdout(ctx)),
      .stderrOutput = kj::str(reprl_fetch_stderr(ctx))};

    KJ_LOG(INFO, "Fuzzout stdout:", result.fuzzout);
    KJ_LOG(INFO, "Workerd stdout:", result.stdoutOutput);
    KJ_LOG(INFO, "Workerd stderr:", result.stderrOutput);

    if (result.wasSignaled()) {
      KJ_LOG(INFO, "Process was terminated by signal", result.termSignal());
    }

    printSplitter();

    return result;
  }

  struct reprl_context* getContext() const { return ctx; }

 private:
  struct reprl_context* ctx;

  KJ_DISALLOW_COPY_AND_MOVE(ReprlContext);
};

void expectSuccess(ReprlContext& reprl, kj::StringPtr code) {
  auto result = reprl.execute(code);
  KJ_EXPECT(result.exitedSuccessfully(), "Execution failed", code);
}

void expectFailure(ReprlContext& reprl, kj::StringPtr code) {
  auto result = reprl.execute(code);
  KJ_EXPECT(!result.exitedSuccessfully(), "Execution unexpectedly succeeded", code);
}

kj::String getRunfilePath(Runfiles& runfiles, const char* rlocationPath) {
  std::string resolved = runfiles.Rlocation(rlocationPath);
  KJ_REQUIRE(!resolved.empty(), "Runfile not found", rlocationPath);
  KJ_REQUIRE(access(resolved.c_str(), F_OK) == 0, "Runfile does not exist", resolved.c_str());
  return kj::str(resolved.c_str());
}

KJ_TEST("REPRL basic functionality") {
  fprintf(stderr, "=== Test started ===\n");
  fflush(stderr);

#ifdef __linux__
  fprintf(stderr, "=== Linux detected, starting REPRL test ===\n");
  fflush(stderr);

  // Create Runfiles object to resolve paths to data dependencies
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));

  kj::String workerdPath;
  kj::String configPath;

  if (runfiles != nullptr) {
    fprintf(stderr, "=== Runfiles created successfully ===\n");
    fflush(stderr);
    // Get absolute paths to workerd binary and config file using runfiles
    workerdPath = getRunfilePath(*runfiles, "_main/src/workerd/server/workerd");
    configPath = getRunfilePath(*runfiles, "_main/samples/reprl/config-full.capnp");
  } else {
    // Fallback for direct execution outside Bazel - use relative paths
    fprintf(stderr, "=== Runfiles not available, using fallback paths ===\n");
    fflush(stderr);
    workerdPath = kj::str("./bazel-bin/src/workerd/server/workerd");
    configPath = kj::str("./samples/reprl/config-full.capnp");

    KJ_REQUIRE(access(workerdPath.cStr(), F_OK) == 0, "Workerd binary not found at", workerdPath);
    KJ_REQUIRE(access(configPath.cStr(), F_OK) == 0, "Config file not found at", configPath);
  }

  fprintf(stderr, "=== Got runfile paths ===\n");
  fflush(stderr);

  KJ_LOG(INFO, "Workerd path:", workerdPath);
  KJ_LOG(INFO, "Config path:", configPath);

  // Create arguments for REPRL
  // Use 'fuzzilli' subcommand (not 'test') for REPRL mode
  auto args = kj::heapArray<const char*>(5);
  args[0] = workerdPath.cStr();
  args[1] = "fuzzilli";
  args[2] = configPath.cStr();
  args[3] = "--experimental";
  args[4] = nullptr;

  fprintf(stderr, "Args prepared: %s %s %s %s\n",
          args[0], args[1], args[2], args[3]);
  fflush(stderr);

  // Build environment for child process
  // Pass LLVM_SYMBOLIZER for symbolization
  // CRITICAL: Set ASAN_OPTIONS to NOT abort on error in the child process.
  // The fuzzilli config sets abort_on_error=1 globally, but the child needs to continue
  // running in the REPRL loop even after errors, so we override it here.
  const char* envVars[] = {
    "LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-19",
    "ASAN_OPTIONS=abort_on_error=0:halt_on_error=0",
    nullptr
  };
  auto env = kj::ArrayPtr<const char* const>(envVars, 3);

  // Initialize REPRL context
  fprintf(stderr, "Creating ReprlContext...\n");
  fflush(stderr);
  ReprlContext reprl;
  fprintf(stderr, "ReprlContext created!\n");
  fflush(stderr);

  fprintf(stderr, "Calling reprl.initialize()...\n");
  fflush(stderr);
  try {
    reprl.initialize(args.asPtr(), env);
    fprintf(stderr, "REPRL initialized successfully!\n");
    fflush(stderr);
  } catch (...) {
    fprintf(stderr, "REPRL initialization failed!\n");
    fprintf(stderr, "Last error: %s\n", reprl_get_last_error(reprl.getContext()));
    fflush(stderr);
    throw;
  }

  // Basic functionality test
  fprintf(stderr, "Executing test script...\n");
  fflush(stderr);
  auto result = reprl.execute("let greeting = \"Hello World!\";");
  fprintf(stderr, "Execution complete! Checking result...\n");
  fflush(stderr);
  KJ_EXPECT(result.exitedSuccessfully(), "Basic script execution failed");
  fprintf(stderr, "Test passed!\n");
  fflush(stderr);
#else
  KJ_LOG(WARNING, "REPRL tests only supported on Linux");
#endif
}

KJ_TEST("REPRL exception handling") {
#ifdef __linux__
  // Create Runfiles object
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));

  kj::String workerdPath;
  kj::String configPath;

  if (runfiles != nullptr) {
    workerdPath = getRunfilePath(*runfiles, "_main/src/workerd/server/workerd");
    configPath = getRunfilePath(*runfiles, "_main/samples/reprl/config-full.capnp");
  } else {
    workerdPath = kj::str("./bazel-bin/src/workerd/server/workerd");
    configPath = kj::str("./samples/reprl/config-full.capnp");
    KJ_REQUIRE(access(workerdPath.cStr(), F_OK) == 0, "Workerd binary not found");
    KJ_REQUIRE(access(configPath.cStr(), F_OK) == 0, "Config file not found");
  }

  auto args = kj::heapArray<const char*>(5);
  args[0] = workerdPath.cStr();
  args[1] = "fuzzilli";  // Use fuzzilli command for REPRL mode
  args[2] = configPath.cStr();
  args[3] = "--experimental";
  args[4] = nullptr;

  // Set environment to prevent ASAN from aborting in the REPRL child
  const char* envVars[] = {
    "LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-19",
    "ASAN_OPTIONS=abort_on_error=0:halt_on_error=0",
    nullptr
  };
  auto env = kj::ArrayPtr<const char* const>(envVars, 3);

  ReprlContext reprl;
  reprl.initialize(args.asPtr(), env);

  // Verify that runtime exceptions can be detected
  expectFailure(reprl, "throw 'failure';");
  expectSuccess(reprl, "42;");

  // Verify that rejected promises are properly reset between executions
  expectFailure(reprl, "function fail() { throw 42; }; fail()");
#else
  KJ_LOG(WARNING, "REPRL tests only supported on Linux");
#endif
}

KJ_TEST("REPRL fuzzilli crash handlers") {
#ifdef __linux__
  // Create Runfiles object
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));

  kj::String workerdPath;
  kj::String configPath;

  if (runfiles != nullptr) {
    workerdPath = getRunfilePath(*runfiles, "_main/src/workerd/server/workerd");
    configPath = getRunfilePath(*runfiles, "_main/samples/reprl/config-full.capnp");
  } else {
    workerdPath = kj::str("./bazel-bin/src/workerd/server/workerd");
    configPath = kj::str("./samples/reprl/config-full.capnp");
    KJ_REQUIRE(access(workerdPath.cStr(), F_OK) == 0, "Workerd binary not found");
    KJ_REQUIRE(access(configPath.cStr(), F_OK) == 0, "Config file not found");
  }

  auto args = kj::heapArray<const char*>(5);
  args[0] = workerdPath.cStr();
  args[1] = "fuzzilli";  // Use fuzzilli command for REPRL mode
  args[2] = configPath.cStr();
  args[3] = "--experimental";
  args[4] = nullptr;

  // Set environment to prevent ASAN from aborting in the REPRL child
  const char* envVars[] = {
    "LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-19",
    "ASAN_OPTIONS=abort_on_error=0:halt_on_error=0",
    nullptr
  };
  auto env = kj::ArrayPtr<const char* const>(envVars, 3);

  ReprlContext reprl;
  reprl.initialize(args.asPtr(), env);

  // Test fuzzilli crash handlers
  expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',0);");
  expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',1);");
  expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',2);");
  expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',3);");
  expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',4);");
  // This one doesn't fail (original comment preserved)
  // expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',5);");
  expectFailure(reprl, "fuzzilli('FUZZILLI_CRASH',6);");
  // async is not failing in workerd (original comment preserved)
  // expectFailure(reprl, "async function fail() { throw 42; }; fail()");
#else
  KJ_LOG(WARNING, "REPRL tests only supported on Linux");
#endif
}

}  // namespace
}  // namespace workerd
