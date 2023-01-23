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

// Stack trace symbolizer. To use link this source to the binary.
// Current implementation shells out to $LLVM_SYMBOLIZER if it is defined.

namespace kj {

namespace {

struct Subprocess {
  // A simple subprocess wrapper with in/out pipes.

  static kj::Maybe<Subprocess> exec(kj::StringPtr cmd) {
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

      execl("/bin/sh", "sh", "-c", cmd.cStr(), nullptr);
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
    return kj::str("\n$LLVM_SYMBOLIZER not defined");
  }

  auto cmd = kj::str(llvmSymbolizer, " --pretty-print --relativenames");
  KJ_IF_MAYBE(subprocess, Subprocess::exec(cmd)) {
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
      KJ_LOG(ERROR, "write error", strerror(errno));
      return nullptr;
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
        KJ_LOG(ERROR, "bad exit code", WEXITSTATUS(status));
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
