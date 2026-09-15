// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Container management API for Durable Object-attached containers.
//
#include <workerd/api/basics.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/container.capnp.h>
#include <workerd/io/io-own.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/strong-bool.h>

namespace workerd::api {

// Whether an exec'd process was started with a PTY allocated. Using a strong bool avoids
// positional-bool mis-wiring at the ExecProcess constructor and exec() call site.
WD_STRONG_BOOL(IsPty);

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

struct ExecPtyOptions {
  // Initial column count for the PTY. If omitted, the runtime default (80) is used.
  jsg::Optional<uint16_t> cols;
  // Initial row count for the PTY. If omitted, the runtime default (24) is used.
  jsg::Optional<uint16_t> rows;

  JSG_STRUCT(cols, rows);
  JSG_STRUCT_TS_OVERRIDE(ContainerExecPtyOptions);
};

struct ExecOptions {
  // $ prefix avoids collision with stdin/stdout/stderr macros from <stdio.h>;
  // JSG_STRUCT strips the $ when exposing to JS.
  jsg::Optional<kj::OneOf<JsReadableStream, kj::String>> $stdin;
  jsg::Optional<kj::String> $stdout;
  jsg::Optional<kj::String> $stderr;
  jsg::Optional<kj::String> cwd;
  jsg::Optional<jsg::Dict<kj::String>> env;
  jsg::Optional<kj::String> user;
  jsg::Optional<jsg::Ref<AbortSignal>> signal;
  // Allocates a PTY for the process. `true` (or an object) enables a PTY with default dimensions;
  // an object may additionally specify initial `cols`/`rows`. Absent or `false` means no PTY.
  jsg::Optional<kj::OneOf<bool, ExecPtyOptions>> pty;

  JSG_STRUCT($stdin, $stdout, $stderr, cwd, env, user, signal, pty);
  JSG_STRUCT_TS_OVERRIDE(ContainerExecOptions {
    stdin?: ReadableStream | "pipe";
    stdout?: "pipe" | "ignore";
    stderr?: "pipe" | "ignore" | "combined";
    cwd?: string;
    env?: Record<string, string>;
    user?: string;
    signal?: AbortSignal;
    pty?: boolean | ContainerExecPtyOptions;
    $stdin: never;
    $stdout: never;
    $stderr: never;
  });
};

class ExecProcess: public jsg::Object {
 public:
  ExecProcess(jsg::Lock& js,
      IoContext& ioContext,
      jsg::Optional<JsWritableStream> stdinStream,
      jsg::Optional<JsReadableStream> stdoutStream,
      jsg::Optional<JsReadableStream> stderrStream,
      int pid,
      rpc::Container::ProcessHandle::Client handle,
      IsPty isPty,
      kj::Maybe<jsg::Ref<AbortSignal>> abortSignal = kj::none);

  jsg::Optional<JsWritableStream> getStdin(jsg::Lock& js);
  jsg::Optional<JsReadableStream> getStdout(jsg::Lock& js);
  jsg::Optional<JsReadableStream> getStderr(jsg::Lock& js);
  int getPid() const {
    return pid;
  }
  // Whether this process was started with a PTY. resize() is only valid when this is true.
  bool getIsPty() const {
    return isPty;
  }
  jsg::MemoizedIdentity<jsg::Promise<int>>& getExitCode(jsg::Lock& js);

  jsg::Promise<jsg::Ref<ExecOutput>> output(jsg::Lock& js);
  void kill(jsg::Lock& js, jsg::Optional<int> signal);
  // Resizes the PTY window to the given dimensions. Throws unless the process was started with a
  // PTY. Both dimensions must be in the range [1, 65535].
  void resize(jsg::Lock& js, int cols, int rows);

  JSG_RESOURCE_TYPE(ExecProcess) {
    JSG_READONLY_PROTOTYPE_PROPERTY(stdin, getStdin);
    JSG_READONLY_PROTOTYPE_PROPERTY(stdout, getStdout);
    JSG_READONLY_PROTOTYPE_PROPERTY(stderr, getStderr);
    JSG_READONLY_PROTOTYPE_PROPERTY(pid, getPid);
    JSG_READONLY_PROTOTYPE_PROPERTY(isPty, getIsPty);
    JSG_READONLY_PROTOTYPE_PROPERTY(exitCode, getExitCode);
    JSG_METHOD(output);
    JSG_METHOD(kill);
    JSG_METHOD(resize);

    JSG_TS_OVERRIDE({
      readonly stdin: WritableStream | null;
      readonly stdout: ReadableStream | null;
      readonly stderr: ReadableStream | null;
      readonly pid: number;
      readonly isPty: boolean;
      readonly exitCode: Promise<number>;
      output(): Promise<ExecOutput>;
      kill(signal?: number): void;
      resize(cols: number, rows: number): void;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_IF_SOME(s, stdinStream) {
      s.visitForMemoryInfo(tracker);
    }
    KJ_IF_SOME(s, stdoutStream) {
      s.visitForMemoryInfo(tracker);
    }
    KJ_IF_SOME(s, stderrStream) {
      s.visitForMemoryInfo(tracker);
    }
    tracker.trackField("exitCodePromise", exitCodePromise);
    tracker.trackField("exitCodePromiseCopy", exitCodePromiseCopy);
  }

 private:
  void ensureExitCodePromise(jsg::Lock& js);
  jsg::Promise<int> getExitCodeForOutput(jsg::Lock& js);

  // Sends a kill signal to the underlying process. Used both by the public kill() method and by
  // the AbortSignal handler.
  void sendKill(int signo);

  jsg::Optional<JsWritableStream> stdinStream;
  jsg::Optional<JsReadableStream> stdoutStream;
  jsg::Optional<JsReadableStream> stderrStream;
  int pid;
  bool isPty;
  IoOwn<rpc::Container::ProcessHandle::Client> handle;
  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<int>>> exitCodePromise;
  kj::Maybe<jsg::Promise<void>> exitCodePromiseCopy;
  kj::Maybe<int> resolvedExitCode;
  bool outputCalled = false;

  // Keeps the kill-on-abort action registered with the exec() options' AbortSignal for as
  // long as this process object is alive.
  kj::Maybe<kj::Own<void>> abortRegistration;

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
  Container(rpc::Container::Client rpcClient,
      bool running,
      jsg::Dict<kj::String> images = jsg::Dict<kj::String>{});

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
    jsg::Optional<DirectorySnapshot> snapshot;
    jsg::Optional<kj::String> mountPoint;

    JSG_STRUCT(snapshot, mountPoint);
    JSG_STRUCT_TS_OVERRIDE(type ContainerDirectorySnapshotRestoreParams =
      | { snapshot: ContainerDirectorySnapshot; mountPoint?: string }
      | { snapshot?: undefined; mountPoint: string }
    );
  };

  struct Snapshot {
    kj::String id;
    double size;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(id, size, name);
  };

  struct SnapshotRestoreParams {
    kj::String id;

    JSG_STRUCT(id);
  };

  struct SnapshotOptions {
    jsg::Optional<kj::String> name;

    JSG_STRUCT(name);
  };

  struct Info {
    jsg::Dict<kj::String> labels;
    kj::String image;

    JSG_STRUCT(labels, image);
  };

  struct StartResources {
    double vcpu;
    double memoryMib;  // Memory size, in MiB (2^20 bytes).
    double diskMb;     // Disk size, in MB (10^6 bytes).

    JSG_STRUCT(vcpu, memoryMib, diskMb);
  };

  struct StartupOptions {
    jsg::Optional<kj::Array<kj::String>> entrypoint;
    bool enableInternet = false;
    jsg::Optional<jsg::Dict<kj::String>> env;
    jsg::Optional<int64_t> hardTimeout;
    jsg::Optional<kj::String> image;
    jsg::Optional<kj::OneOf<kj::String, StartResources>> instance;
    jsg::Optional<jsg::Dict<kj::String>> labels;
    jsg::Optional<kj::Array<DirectorySnapshotRestoreParams>> directorySnapshots;
    jsg::Optional<SnapshotRestoreParams> containerSnapshot;

    // TODO(containers): Allow intercepting stdin/stdout/stderr by specifying streams here.

    JSG_STRUCT(entrypoint,
        enableInternet,
        env,
        hardTimeout,
        image,
        instance,
        labels,
        directorySnapshots,
        containerSnapshot);
    JSG_STRUCT_TS_OVERRIDE_DYNAMIC(CompatibilityFlags::Reader flags) {
      if (flags.getWorkerdExperimental()) {
        JSG_TS_OVERRIDE(type ContainerStartupOptions = {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          hardTimeout?: number | bigint;
          instance?: "lite" | "standard-1" | "standard-2" | "standard-3" | "standard-4" | ContainerStartResources;
          labels?: Record<string, string>;
          directorySnapshots?: ContainerDirectorySnapshotRestoreParams[];
        } & (
          | {
              /** Cannot be used with `containerSnapshot`. */
              image: string;
              containerSnapshot?: never;
            }
          | {
              image?: never;
              /** Cannot be used with `image`. */
              containerSnapshot?: ContainerSnapshotRestoreParams;
            }
        ));
      } else {
        JSG_TS_OVERRIDE(type ContainerStartupOptions = {
          entrypoint?: string[];
          enableInternet: boolean;
          env?: Record<string, string>;
          instance?: "lite" | "standard-1" | "standard-2" | "standard-3" | "standard-4" | ContainerStartResources;
          labels?: Record<string, string>;
          directorySnapshots?: ContainerDirectorySnapshotRestoreParams[];
        } & (
          | {
              /** Cannot be used with `containerSnapshot`. */
              image: string;
              containerSnapshot?: never;
            }
          | {
              image?: never;
              /** Cannot be used with `image`. */
              containerSnapshot?: ContainerSnapshotRestoreParams;
            }
        ));
      }
    }
  };

  bool getRunning();
  jsg::Dict<kj::String> getImages() const;

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

  jsg::Promise<void> setLabels(jsg::Lock& js, jsg::Dict<kj::String> labels);

  // TODO(containers): listenTcp()

  JSG_RESOURCE_TYPE(Container, CompatibilityFlags::Reader flags) {
    JSG_READONLY_PROTOTYPE_PROPERTY(running, getRunning);
    JSG_READONLY_PROTOTYPE_PROPERTY(images, getImages);
    JSG_METHOD(start);
    JSG_METHOD(monitor);
    JSG_METHOD(destroy);
    JSG_METHOD(signal);
    JSG_METHOD(getTcpPort);
    JSG_METHOD(setInactivityTimeout);

    JSG_METHOD(interceptOutboundHttp);
    JSG_METHOD(interceptAllOutboundHttp);
    JSG_METHOD(snapshotContainer);
    JSG_METHOD(interceptOutboundHttps);
    JSG_METHOD(exec);
    JSG_METHOD(inspect);
    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(interceptOutboundTcp);
      JSG_METHOD(snapshotDirectory);
      JSG_METHOD(setLabels);
    }
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("destroyReason", destroyReason);
    tracker.trackField("images", images);
  }

 private:
  IoOwn<rpc::Container::Client> rpcClient;
  jsg::Dict<kj::String> images;

  struct Monitor final {
    Monitor(kj::ForkedPromise<int32_t> promise, uint64_t generation);

    bool running = true;

    // Identifies callbacks belonging to this lifecycle.
    // Callers can use this to compare with the current generation.
    uint64_t generation;

    // Shares the container exit result with explicit monitor() calls.
    kj::ForkedPromise<int32_t> promise;

    // Resolves after running has been set to false.
    // It is useful for things like destroy() needing to await on
    // running becoming false.
    kj::ForkedPromise<void> stopped;

    // Drives stopped even when nothing is explicitly waiting on it.
    // Eagerly evaluated from `stopped`.
    kj::Promise<void> background;

   private:
    static kj::Promise<void> observe(kj::ForkedPromise<int32_t>& promise, bool& running);
  };

  // Shares one monitor RPC between the background state update and explicit monitor() calls.
  kj::Maybe<IoOwn<Monitor>> currentMonitor;
  uint64_t nextMonitorGeneration = 0;

  kj::Maybe<jsg::Value> destroyReason;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(destroyReason);
  }

  class TcpPortWorkerInterface;
  class TcpPortOutgoingFactory;

  // Per-TCP-port state for the tunnel-reuse optimization. Populated lazily by getTcpPort() when
  // the container-tunnel-reuse autogate is enabled. Held via IoOwn because it holds KJ I/O objects
  // (Cap'n Proto capabilities, kj streams) that must remain tied to the Durable Object's IoContext.
  class TcpPortState;
  kj::Maybe<IoOwn<kj::HashMap<int, kj::Rc<TcpPortState>>>> tcpPortStates;

  void invalidateTcpPortStates();
  void startMonitor();
  bool isCurrentMonitor(uint64_t generation);

  // These helpers are static since they will leave the IoContext on the first co_await, so we
  // don't want them trying to access `rpcClient` via the `IoOwn`.
  static kj::Promise<void> interceptOutboundHttpImpl(rpc::Container::Client rpcClient,
      kj::String addr,
      kj::Own<IoChannelFactory::SubrequestChannel> channel);
  static kj::Promise<void> interceptAllOutboundHttpImpl(
      rpc::Container::Client rpcClient, kj::Own<IoChannelFactory::SubrequestChannel> channel);
  static kj::Promise<void> interceptOutboundHttpsImpl(rpc::Container::Client rpcClient,
      kj::String addr,
      kj::Own<IoChannelFactory::SubrequestChannel> channel);
  static kj::Promise<void> interceptOutboundTcpImpl(rpc::Container::Client rpcClient,
      kj::String addr,
      kj::Own<IoChannelFactory::SubrequestChannel> channel);
};

#define EW_CONTAINER_ISOLATE_TYPES                                                                 \
  api::ExecOutput, api::ExecOptions, api::ExecPtyOptions, api::ExecProcess, api::Container,        \
      api::Container::DirectorySnapshot, api::Container::DirectorySnapshotOptions,                 \
      api::Container::DirectorySnapshotRestoreParams, api::Container::Snapshot,                    \
      api::Container::SnapshotRestoreParams, api::Container::SnapshotOptions,                      \
      api::Container::StartupOptions, api::Container::Info, api::Container::StartResources

}  // namespace workerd::api
