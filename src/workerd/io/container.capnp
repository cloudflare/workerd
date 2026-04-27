@0xcb7be0e1be835084;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");
$Cxx.allowCancellation;

using import "/capnp/compat/byte-stream.capnp".ByteStream;
using CompatibilityFlags = import "/workerd/io/compatibility-date.capnp".CompatibilityFlags;

interface Container @0x9aaceefc06523bca {
  # RPC interface to talk to a container, for containers attached to Durable Objects.
  #
  # When the actor shuts down, workerd will drop the `Container` capability, at which point
  # the container engine should implicitly destroy the container.

  status @0 () -> (running :Bool);
  # Returns the container's current status. The runtime will always call this at DO startup.

  start @1 StartParams -> ();
  # Start the container. It's an error to call this if the container is already running.

  struct StartParams {
    entrypoint @0 :List(Text);
    # Specifies the command to run as the root process of the container. If null, the container
    # image's default command is used.

    enableInternet @1 :Bool = false;
    # Set true to enable the container to talk directly to the public internet. Otherwise, the
    # public internet will not be accessible -- but it's still possible to intercept connection
    # attempts and handle them in the DO, using the `listenTcp()` method below.

    environmentVariables @2 :List(Text);
    # Specifies the environment variables of the container.
    # It will spread over the existing defined environment variables of the container image.
    # If null, the container will start with the environment variables defined in its image.
    # The format is defined as a list of `NAME=VALUE`.
    # The container runtime should validate the environment variables input.

    hardTimeoutMs @3 :Int64;
    # Configures an absolute timeout that starts when the container starts and never resets.
    # The container will be forcefully terminated when this timeout expires, regardless of activity.
    # Unlike inactivity timeout, this is a hard deadline from container startup.
    # If 0 (default), no hard timeout is applied.

    compatibilityFlags @4 :CompatibilityFlags;
    # Compatibility flags for this worker

    labels @5 :List(Label);
    # Optional key-value metadata labels for metrics/observability.

    directorySnapshots @6 :List(DirectorySnapshotRestoreParams);
    # Directory snapshots to restore before the container starts.

    containerSnapshotId @7 :Text;
    # Id of the full container snapshot to restore before the container starts.
  }

  struct Label {
    name @0 :Text;
    value @1 :Text;
  }

  struct DirectorySnapshotRestoreParams {
    snapshotId @0 :Text;
    # The id of the snapshot to restore.

    restorePath @1 :Text;
    # Where to restore the snapshot in the container filesystem.
  }

  struct DirectorySnapshot {
    # Opaque handle to a directory snapshot.

    id @0 :Text;
    # Unique identifier of the snapshot.

    size @1 :UInt64;
    # Snapshot size, in bytes.

    dir @2 :Text;
    # Path of the snapshotted directory.

    name @3 :Text;
    # Optional human-friendly name. Empty string means not set.
  }

  struct SnapshotDirectoryParams {
    dir @0 :Text;
    # Directory path to snapshot.

    name @1 :Text;
    # Optional human-friendly name. Empty string means not set.
  }

  struct ContainerSnapshot {
    # Opaque handle to a full container snapshot.

    id @0 :Text;
    # Unique identifier of the snapshot.

    size @1 :UInt64;
    # Snapshot size, in bytes.

    name @2 :Text;
    # Optional human-friendly name. Empty string means not set.
  }

  struct SnapshotContainerParams {
    name @0 :Text;
    # Optional human-friendly name. Empty string means not set.
  }

  struct ExecOptions {
    env @0 :List(Text);
    # Environment variables to add/override for the exec'd process, in NAME=VALUE format.

    workingDirectory @1 :Text;
    # Working directory for the exec'd process. Empty string means use the container default.

    user @2 :Text;
    # User for the exec'd process. Empty string means use the container default.

    combinedOutput @3 :Bool;
    # If true, stderr is combined into stdout. If stdout is not set, combined output is discarded.
  }

  struct Process {
    pid @0 :Int32;
    handle @1 :ProcessHandle;
  }

  interface ProcessHandle {
    wait @0 () -> (exitCode :Int32);
    # Waits for the process to exit and returns its exit code.

    stdinWriter @1 () -> (writer :ByteStream);
    # Retrieves a ByteStream handle to write to the process's stdin.
    # If not called before wait(), stdin automatically EOFs.
    # Throws an error if called after wait().

    kill @2 (signo :UInt32);
    # Sends the given signal to the process.
  }

  monitor @2 () -> (exitCode: Int32);
  # Waits for the container to shut down.
  #
  # If the container shuts down because the root process exited with a success status, or because
  # the client invoked `destroy()`, then `monitor()` completes without an error. If it shuts down
  # for any other reason, `monitor()` throws an exception describing what happened. (This exception
  # may or may not be a JSG exception depending on whether it is an application error or a system
  # error.)

  destroy @3 ();
  # Immediately and abruptly stops the container and tears it down. The application is not given
  # any warning, it simply stops immediately. Upon successful return from destroy(), the container
  # is no longer running. If a call to `monitor()` is waiting when `destroy()` is invoked,
  # `monitor()` will also return (with no error). If the container is not running when `destroy()`
  # is invoked, `destroy()` silently returns with no error.

  signal @4 (signo :UInt32);
  # Sends the given Linux signal number to the root process.

  getTcpPort @5 (port :UInt16) -> (port :Port);
  # Obtains an object which can be used to connect to the application inside the container on the
  # given TCP port (the application must be listening on this port).

  interface Port {
    # Represents a port to which connections can be made.

    connect @0 (down :ByteStream) -> (up :ByteStream);
    # Forms a raw socket connection to the port.
    #
    # Note that when the Durable Object application uses the HTTP-oriented APIs, workerd will
    # take care of speaking the HTTP protocol on top of the raw socket. So, the container engine
    # need only implement raw connections.
  }

  listenTcp @6 (filter :IpFilter, handler :TcpHandler) -> (handle :Capability);
  # Arranges to intercept outgoing TCP connections from the container and redirect them to the
  # given `handler`.

  struct IpFilter {
    # Specifies a range of IP addresses and/or port numbers which should be intercepted when the
    # application in the container tries to connect to them.

    addr @0 :Text;
    # null = all addresses
    # TODO(someday): Support CIDR? (e.g. "192.168.0.0/16")

    port @1 :UInt16 = 0;
    # 0 = all ports
  }

  interface TcpHandler {
    # Interface which intercepts outgoing connections from a container.

    connect @0 (addr :Text, port :UInt16, down :ByteStream) -> (up :ByteStream);
    # Like Port.connect() but also receives the address and port number to which the container was
    # attempting to connect.
  }

  setInactivityTimeout @7 (durationMs  :Int64);
  # Configures the duration where the runtime should shutdown the container after there is
  # no connections or activity to the Container.
  #
  # After a capability disconnect, the runtime should signal the container
  # at the configured duration.
  #
  # Note that if there is an open connection to the container, the runtime must not shutdown the container.
  # If there is no activity timeout duration configured and no container connection, it's up to the runtime
  # to decide when to signal the container to exit.

  setEgressHttp @8 (hostPort :Text, channelToken :Data);
  # Configures egress HTTP routing for the container. When the container attempts to connect to the
  # specified host:port, the connection should be routed back to the Workers runtime using the channel token.
  # The format of hostPort can be '<ip|cidr|hostnameGlob>[':'<port>]'. If the host part is not an
  # IP or CIDR, it is treated as a hostname glob.
  # If port is omitted, it's assumed to only cover port 80.
  # This method does not support HTTPs yet.

  setEgressHttps @9 (hostPort :Text, channelToken :Data);
  # Configures egress HTTPS routing for the container. The format of `hostPort` is the same as
  # `setEgressHttp`: '<ip|cidr|hostnameGlob>[':'<port>]'. If the host part is not an IP or CIDR,
  # it is treated as a hostname glob matched against the TLS SNI hostname. If `port` is omitted,
  # it is assumed to only cover port 443.
  #
  # The runtime routes matching decrypted HTTP traffic back to Workers using `channelToken` and
  # must ensure the container trusts the interception CA.

  setEgressTcp @13 (hostPort :Text, channelToken :Data);
  # Configures egress TCP routing for the container. When the container attempts a raw TCP
  # connection to the specified host:port, the connection is routed back to the Workers
  # runtime using the channel token. The worker binding is expected to implement a connect()
  # handler that receives the destination address and a bidirectional byte stream.
  # The format of `hostPort` is '<ip|cidr>[':'<port>]'. Hostname glob matching is not
  # supported for TCP since there is no application-layer hostname information. If `port`
  # is omitted, all ports are matched.

  snapshotDirectory @10 SnapshotDirectoryParams -> (snapshot :DirectorySnapshot);
  # Creates a snapshot for a directory in the running container.

  snapshotContainer @11 SnapshotContainerParams -> (snapshot :ContainerSnapshot);
  # Creates a full container snapshot for the running container.

  exec @12 (cmd :List(Text), stdoutWriter :ByteStream, stderrWriter :ByteStream,
      params :ExecOptions) -> (process :Process);
  # Executes a short-lived process in the running container.
  #
  # If stdoutWriter/stderrWriter are not provided, output is discarded. If params.combinedOutput
  # is true, stderr is merged into stdout and the stderrWriter capability is ignored.

  inspect @14 () -> (info :InspectInfo);
  # Returns information about the container, or `none` if the container has not been started.

  struct InspectInfo {
    union {
      none @0 :Void;
      started :group {
        labels @1 :List(Label);
        # Echo of StartParams.labels. Empty list means start() was called with no labels.
      }
    }
  }
}
