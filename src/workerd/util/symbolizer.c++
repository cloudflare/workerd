// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#ifndef EKAM_BUILD

#include <cstdint>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include "dlfcn.h"
#include <workerd/util/sentry.h>

// Stack trace symbolizer. To use link this source to the binary.
// Current implementation shells out to $LLVM_SYMBOLIZER if it is defined.

namespace kj {

namespace {

struct Subprocess {
  // A simple subprocess wrapper with in/out pipes.

  static kj::Maybe<Subprocess> exec(const char* argv[]) noexcept {
    // Execute command with a shell.
    // Since this is used during error handling we do not to try to free resources in
    // case of errors.

    int in[2]; // process stdin pipe
    int out[2]; // process stdout pipe

    if (pipe(in)) {
      KJ_LOG(ERROR, "can't allocate in pipe", strerror(errno));
      return nullptr;
    }
    if (pipe(out)){
      KJ_LOG(ERROR, "can't allocate out pipe", strerror(errno));
      return nullptr;
    }

    auto pid = fork();
    if (pid > 0) {
      // parent
      close(in[0]);
      close(out[1]);
      return Subprocess({ .pid = pid, .in = in[1], .out = out[0] });
    } else {
      // child
      close(in[1]);
      close(out[0]);

      // redirect stdin
      close(0);
      if(dup(in[0]) < 0) {
        _exit(2 * errno);
      }

      // redirect stdout
      close(1);
      if(dup(out[1]) < 0) {
        _exit(3 * errno);
      }

      KJ_SYSCALL_HANDLE_ERRORS(execvp(argv[0], const_cast<char**>(argv))) {
        case ENOENT:
          _exit(2);
        default:
          KJ_FAIL_SYSCALL("execvp", error);
      }
      _exit(1);
    }
  }

  int closeAndWait() {
    close(in);
    close(out);
    int status;
    waitpid(pid, &status, 0);
    return status;
  }

  int pid;
  int in;
  int out;
};

} // namespace

String stringifyStackTrace(ArrayPtr<void* const> trace) {
  const char* llvmSymbolizer = getenv("LLVM_SYMBOLIZER");
  if (llvmSymbolizer == nullptr) {
    LOG_WARNING_ONCE("Not symbolizing stack traces because $LLVM_SYMBOLIZER is not set. "
        "To symbolize stack traces, set $LLVM_SYMBOLIZER to the location of the llvm-symbolizer "
        "binary. When running tests under bazel, use `--test_env=LLVM_SYMBOLIZER=<path>`.");
    return nullptr;
  }

  const char* argv[] = {
    llvmSymbolizer,
    "--pretty-print",
    "--relativenames",
    nullptr
  };

  bool disableSigpipeOnce KJ_UNUSED = []() {
    // Just in case for some reason SIGPIPE is not already disabled in this process, disable it
    // now, otherwise the below will fail in the case that llvm-symbolizer is not found. Note
    // that if the process creates a kj::UnixEventPort, then SIGPIPE will already be disabled.
    signal(SIGPIPE, SIG_IGN);
    return false;
  }();

  KJ_IF_MAYBE(subprocess, Subprocess::exec(argv)) {
    // write addresses as "CODE <file_name> <hex_address>" lines.
    auto addrs = strArray(KJ_MAP(addr, trace) {
      Dl_info info;
      if (dladdr(addr, &info)) {
        uintptr_t offset = reinterpret_cast<uintptr_t>(addr) -
                            reinterpret_cast<uintptr_t>(info.dli_fbase);
        return kj::str("CODE ", info.dli_fname, " 0x", reinterpret_cast<void*>(offset));
      } else {
        return kj::str("CODE 0x", reinterpret_cast<void*>(addr));
      }
    }, "\n");
    if (write(subprocess->in, addrs.cStr(), addrs.size()) != addrs.size()) {
      // Ignore EPIPE, which means the process exited early. We'll deal with it below, presumably.
      if (errno != EPIPE) {
        KJ_LOG(ERROR, "write error", strerror(errno));
        return nullptr;
      }
    }
    close(subprocess->in);

    // read result
    auto out = fdopen(subprocess->out, "r");
    if (!out) {
      KJ_LOG(ERROR, "fdopen error", strerror(errno));
      return nullptr;
    }

    kj::String lines[256];
    size_t i = 0;
    for (char line[512]; fgets(line, sizeof(line), out) != nullptr;) {
      if (i < kj::size(lines)) {
        lines[i++] = kj::str(line);
      }
    }
    int status = subprocess->closeAndWait();
    if (WIFEXITED(status)) {
      if (WEXITSTATUS(status) != 0) {
        if (WEXITSTATUS(status) == 2) {
          LOG_WARNING_ONCE(kj::str(llvmSymbolizer, " was not found. "
              "To symbolize stack traces, install it in your $PATH or set $LLVM_SYMBOLIZER to the "
              "location of the binary. When running tests under bazel, use "
              "`--test_env=LLVM_SYMBOLIZER=<path>`."));
        } else {
          KJ_LOG(ERROR, "bad exit code", WEXITSTATUS(status));
        }
        return nullptr;
      }
    } else {
      KJ_LOG(ERROR, "bad exit status", status);
      return nullptr;
    }
    return kj::str("\n", kj::strArray(kj::arrayPtr(lines, i), ""));
  } else {
    return kj::str("\nerror starting llvm-symbolizer");
  }
}

} // kj

#endif  // EKAM_BUILD
