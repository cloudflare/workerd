#include <kj/debug.h>
#include <kj/exception.h>
#include <kj/memory.h>
#include <kj/string.h>
#include <kj/vector.h>

// Libreprl is a .c file so the header needs to be in an 'extern "C"' block.
extern "C" {
#include "libreprl/libreprl.h"
}  // extern "C"

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

  ExecutionResult execute(kj::StringPtr code) {
    uint64_t execTime;
    const uint64_t TIMEOUT_SECONDS = 1000000;  // Timeout is in microseconds.

    printSplitter();
    KJ_LOG(INFO, "Executing:", code);

    int status = reprl_execute(ctx, code.cStr(), code.size(), TIMEOUT_SECONDS, &execTime, 0);

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

 private:
  struct reprl_context* ctx;

  KJ_DISALLOW_COPY_AND_MOVE(ReprlContext);
};

void expectSuccess(ReprlContext& reprl, kj::StringPtr code) {
  auto result = reprl.execute(code);
  KJ_REQUIRE(result.exitedSuccessfully(), "Execution failed", code);
}

void expectFailure(ReprlContext& reprl, kj::StringPtr code) {
  auto result = reprl.execute(code);
  KJ_REQUIRE(!result.exitedSuccessfully(), "Execution unexpectedly succeeded", code);
}

}  // namespace

int main(int argc, char** argv) try {
#ifdef __linux__
  // Use KJ-style argument validation
  KJ_REQUIRE(
      argc >= 4, "Usage:", argv[0], "<workerd_path> <command> <path-to-config> <workerd-flags>");

  // Create KJ arrays for arguments and environment
  auto args = kj::heapArray<const char*>(argc - 1);
  for (int i = 1; i < argc; i++) {
    args[i - 1] = argv[i];
  }
  args.back() = nullptr;

  const char* envVars[] = {"LLVM_SYMBOLIZER=/usr/bin/llvm-symbolizer-19", nullptr};
  auto env = kj::ArrayPtr<const char* const>(envVars, 2);

  // Initialize REPRL context with KJ-style error handling
  ReprlContext reprl;
  reprl.initialize(args.asPtr(), env);

  // Basic functionality test
  auto result = reprl.execute("let greeting = \"Hello World!\";");
  KJ_REQUIRE(result.exitedSuccessfully(), "Basic script execution failed");

  // Verify that runtime exceptions can be detected
  expectFailure(reprl, "throw 'failure';");
  expectSuccess(reprl, "42;");

  // Verify that existing state is properly reset between executions
  // These tests are commented out in original, keeping as-is
  // expectSuccess(reprl, "globalProp = 42; Object.prototype.foo = \"bar\";");
  // expectSuccess(reprl, "if (typeof(globalProp) !== 'undefined') throw 'failure'");
  // expectSuccess(reprl, "if (typeof(({}).foo) !== 'undefined') throw 'failure'");

  // Verify that rejected promises are properly reset between executions
  expectFailure(reprl, "function fail() { throw 42; }; fail()");

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

  KJ_LOG(INFO, "All tests passed successfully!");
#else
  KJ_LOG(WARNING, "REPRL tests only supported on Linux");
#endif
  return 0;
} catch (const kj::Exception& e) {
  KJ_LOG(ERROR, "Test failed with exception:", e);
  return 1;
}
