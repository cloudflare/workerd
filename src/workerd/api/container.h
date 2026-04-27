// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Container management API for Durable Object-attached containers.
//
#include <workerd/api/streams/readable.h>
#include <workerd/api/streams/writable.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/container.capnp.h>
#include <workerd/io/io-own.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Fetcher;
class ExecOutput: public jsg::Object {
 public:
  ExecOutput(kj::Array<kj::byte> stdoutBytes, kj::Array<kj::byte> stderrBytes, int exitCode);

  jsg::JsArrayBuffer getStdout(jsg::Lock& js);
  jsg::JsArrayBuffer getStderr(jsg::Lock& js);
  int getExitCode() const {
    return exitCode;
  }

  JSG_RESOURCE_TYPE(ExecOutput) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(stdout, getStdout);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(stderr, getStderr);
    JSG_READONLY_PROTOTYPE_PROPERTY(exitCode, getExitCode);

    JSG_TS_OVERRIDE({
      readonly stdout: ArrayBuffer;
      readonly stderr: ArrayBuffer;
      readonly exitCode: number;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("stdout", stdoutBytes);
    tracker.trackField("stderr", stderrBytes);
  }

 private:
  kj::Array<kj::byte> stdoutBytes;
  kj::Array<kj::byte> stderrBytes;
  int exitCode;
};

struct ExecOptions {
  // $ prefix avoids collision with stdin/stdout/stderr macros from <stdio.h>;
  // JSG_STRUCT strips the $ when exposing to JS.
  jsg::Optional<kj::OneOf<jsg::Ref<ReadableStream>, kj::String>> $stdin;
  jsg::Optional<kj::String> $stdout;
  jsg::Optional<kj::String> $stderr;
  jsg::Optional<kj::String> cwd;
  jsg::Optional<jsg::Dict<kj::String>> env;
  jsg::Optional<kj::String> user;

  JSG_STRUCT($stdin, $stdout, $stderr, cwd, env, user);
  JSG_STRUCT_TS_OVERRIDE(ContainerExecOptions {
    stdin?: ReadableStream | "pipe";
    stdout?: "pipe" | "ignore";
    stderr?: "pipe" | "ignore" | "combined";
    cwd?: string;
    env?: Record<string, string>;
    user?: string;
    $stdin: never;
    $stdout: never;
    $stderr: never;
  });
};

class ExecProcess: public jsg::Object {
 public:
  ExecProcess(jsg::Optional<jsg::Ref<WritableStream>> stdinStream,
      jsg::Optional<jsg::Ref<ReadableStream>> stdoutStream,
      jsg::Optional<jsg::Ref<ReadableStream>> stderrStream,
      int pid,
      rpc::Container::ProcessHandle::Client handle);

  jsg::Optional<jsg::Ref<WritableStream>> getStdin();
  jsg::Optional<jsg::Ref<ReadableStream>> getStdout();
  jsg::Optional<jsg::Ref<ReadableStream>> getStderr();
  int getPid() const {
    return pid;
  }
  jsg::MemoizedIdentity<jsg::Promise<int>>& getExitCode(jsg::Lock& js);

  jsg::Promise<jsg::Ref<ExecOutput>> output(jsg::Lock& js);
  void kill(jsg::Lock& js, jsg::Optional<int> signal);

  JSG_RESOURCE_TYPE(ExecProcess) {
    JSG_READONLY_PROTOTYPE_PROPERTY(stdin, getStdin);
    JSG_READONLY_PROTOTYPE_PROPERTY(stdout, getStdout);
    JSG_READONLY_PROTOTYPE_PROPERTY(stderr, getStderr);
    JSG_READONLY_PROTOTYPE_PROPERTY(pid, getPid);
    JSG_READONLY_PROTOTYPE_PROPERTY(exitCode, getExitCode);
    JSG_METHOD(output);
    JSG_METHOD(kill);

    JSG_TS_OVERRIDE({
      readonly stdin: WritableStream | null;
      readonly stdout: ReadableStream | null;
      readonly stderr: ReadableStream | null;
      readonly pid: number;
      readonly exitCode: Promise<number>;
      output(): Promise<ExecOutput>;
      kill(signal?: number): void;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("stdin", stdinStream);
    tracker.trackField("stdout", stdoutStream);
    tracker.trackField("stderr", stderrStream);
    tracker.trackField("exitCodePromise", exitCodePromise);
    tracker.trackField("exitCodePromiseCopy", exitCodePromiseCopy);
  }

 private:
  void ensureExitCodePromise(jsg::Lock& js);
  jsg::Promise<int> getExitCodeForOutput(jsg::Lock& js);

  jsg::Optional<jsg::Ref<WritableStream>> stdinStream;
  jsg::Optional<jsg::Ref<ReadableStream>> stdoutStream;
  jsg::Optional<jsg::Ref<ReadableStream>> stderrStream;
  int pid;
  IoOwn<rpc::Container::ProcessHandle::Client> handle;
  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<int>>> exitCodePromise;
  kj::Maybe<jsg::Promise<void>> exitCodePromiseCopy;
  kj::Maybe<int> resolvedExitCode;
  bool outputCalled = false;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(stdinStream, stdoutStream, stderrStream, exitCodePromise, exitCodePromiseCopy);
  }
};

// Implements the `ctx.container` API for durable-object-attached containers. This API allows
// the DO to supervise the attached container (lightweight virtual machine), including starting,
// stopping, monitoring, making requests to the container, intercepting outgoing network requests,
// etc.
class Container: public jsg::Object {
 public:
  Container(rpc::Container::Client rpcClient, bool running);

  struct DirectorySnapshot {
    kj::String id;
    double size;
    kj::String dir;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(id, size, dir, name);
  };

  struct DirectorySnapshotOptions {
    kj::String dir;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(dir, name);
  };

  struct DirectorySnapshotRestoreParams {
    DirectorySnapshot snapshot;
    jsg::Optional<kj::String> mountPoint;

    JSG_STRUCT(snapshot, mountPoint);
  };

  struct Snapshot {
    kj::String id;
    double size;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(id, size, name);
  };

  struct SnapshotOptions {
    jsg::Optional<kj::String> name;

    JSG_STRUCT(name);
  };

  struct Info {
    jsg::Dict<kj::String> labels;

    JSG_STRUCT(labels);
  };

  struct StartupOptions {
    jsg::Optional<kj::Array<kj::String>> entrypoint;
    bool enableInternet = false;
    jsg::Optional<jsg::Dict<kj::String>> env;
    jsg::Optional<int64_t> hardTimeout;
    jsg::Optional<jsg::Dict<kj::String>> labels;
    jsg::Optional<kj::Array<DirectorySnapshotRestoreParams>> directorySnapshots;
    jsg::Optional<Snapshot> containerSnapshot;

    // TODO(containers): Allow intercepting stdin/stdout/stderr by specifying streams here.

    JSG_STRUCT(entrypoint,
        enableInternet,
        env,
        hardTimeout,
        labels,
        directorySnapshots,
        containerSnapshot);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: number | bigint;
          labels?: Record<string, string>;
          directorySnapshots?: ContainerDirectorySnapshotRestoreParams[];
          containerSnapshot?: ContainerSnapshot;
        });
      } else {
        JSG_TS_OVERRIDE(ContainerStartupOptions {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: never;
          labels?: Record<string, string>;
          directorySnapshots?: ContainerDirectorySnapshotRestoreParams[];
          containerSnapshot?: ContainerSnapshot;
        });
      }
    }
  };

  bool getRunning() const {
    return running;
  }

  // Methods correspond closely to the RPC interface in `container.capnp`.
  void start(jsg::Lock& js, jsg::Optional<StartupOptions> options);
  jsg::Promise<void> monitor(jsg::Lock& js);
  jsg::Promise<void> destroy(jsg::Lock& js, jsg::Optional<jsg::Value> error);
  void signal(jsg::Lock& js, int signo);
  jsg::Ref<Fetcher> getTcpPort(jsg::Lock& js, int port);
  jsg::Promise<void> setInactivityTimeout(jsg::Lock& js, int64_t durationMs);
  jsg::Promise<void> interceptOutboundHttp(
      jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding);
  jsg::Promise<void> interceptAllOutboundHttp(jsg::Lock& js, jsg::Ref<Fetcher> binding);
  jsg::Promise<void> interceptOutboundHttps(
      jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding);
  jsg::Promise<void> interceptOutboundTcp(
      jsg::Lock& js, kj::String addr, jsg::Ref<Fetcher> binding);
  jsg::Promise<DirectorySnapshot> snapshotDirectory(
      jsg::Lock& js, DirectorySnapshotOptions options);
  jsg::Promise<Snapshot> snapshotContainer(jsg::Lock& js, SnapshotOptions options);
  jsg::Promise<jsg::Ref<ExecProcess>> exec(
      jsg::Lock& js, kj::Array<kj::String> cmd, jsg::Optional<ExecOptions> options);

  jsg::Promise<kj::Maybe<Info>> inspect(jsg::Lock& js);

  // TODO(containers): listenTcp()

  JSG_RESOURCE_TYPE(Container, CompatibilityFlags::Reader flags) {
    JSG_READONLY_PROTOTYPE_PROPERTY(running, getRunning);
    JSG_METHOD(start);
    JSG_METHOD(monitor);
    JSG_METHOD(destroy);
    JSG_METHOD(signal);
    JSG_METHOD(getTcpPort);
    JSG_METHOD(setInactivityTimeout);

    JSG_METHOD(interceptOutboundHttp);
    JSG_METHOD(interceptAllOutboundHttp);
    JSG_METHOD(snapshotDirectory);
    JSG_METHOD(snapshotContainer);
    JSG_METHOD(interceptOutboundHttps);
    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(exec);
      JSG_METHOD(interceptOutboundTcp);
      JSG_METHOD(inspect);
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("destroyReason", destroyReason);
  }

 private:
  IoOwn<rpc::Container::Client> rpcClient;
  bool running;

  kj::Maybe<jsg::Value> destroyReason;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(destroyReason);
  }

  class TcpPortWorkerInterface;
  class TcpPortOutgoingFactory;
};

#define EW_CONTAINER_ISOLATE_TYPES                                                                 \
  api::ExecOutput, api::ExecOptions, api::ExecProcess, api::Container,                             \
      api::Container::DirectorySnapshot, api::Container::DirectorySnapshotOptions,                 \
      api::Container::DirectorySnapshotRestoreParams, api::Container::Snapshot,                    \
      api::Container::SnapshotOptions, api::Container::StartupOptions, api::Container::Info

}  // namespace workerd::api
