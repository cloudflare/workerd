# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xfafd0bdf57acc528;
using Cxx = import "/capnp/c++.capnp";
using Json = import "/capnp/compat/json.capnp";

$Cxx.namespace("workerd::docker_api");
$Cxx.allowCancellation;

struct Docker {
  # Docker API structures for container operations
  struct PortBinding {
    hostIp @0 :Text $Json.name("HostIp");
    hostPort @1 :Text $Json.name("HostPort");
  }

  struct LogConfig {
    type @0 :Text $Json.name("Type");
    config @1 :Json.Value $Json.name("Config");
  }

  struct RestartPolicy {
    name @0 :Text $Json.name("Name"); # "no", "always", "unless-stopped", "on-failure"
    maximumRetryCount @1 :UInt32 $Json.name("MaximumRetryCount");
  }

  struct WeightDevice {
    path @0 :Text;
    weight @1 :UInt16;
  }

  struct ThrottleDevice {
    path @0 :Text;
    rate @1 :UInt64;
  }

  struct DeviceMapping {
    pathOnHost @0 :Text;
    pathInContainer @1 :Text;
    cgroupPermissions @2 :Text;
  }

  struct DeviceRequest {
    driver @0 :Text;
    count @1 :Int32;
    deviceIds @2 :List(Text);
    capabilities @3 :List(List(Text));
    options @4 :Json.Value;
  }

  struct Ulimit {
    name @0 :Text;
    soft @1 :UInt64;
    hard @2 :UInt64;
  }

  struct IPAMConfig {
    ipv4Address @0 :Text $Json.name("IPv4Address");
    ipv6Address @1 :Text $Json.name("IPv6Address");
    linkLocalIps @2 :List(Text) $Json.name("LinkLocalIPs");
  }

  struct EndpointSettings {
    ipamConfig @0 :IPAMConfig $Json.name("IPAMConfig");
    links @1 :List(Text) $Json.name("Links");
    aliases @2 :List(Text) $Json.name("Aliases");
    networkId @3 :Text $Json.name("NetworkID");
    endpointId @4 :Text $Json.name("EndpointID");
    gateway @5 :Text $Json.name("Gateway");
    ipAddress @6 :Text $Json.name("IPAddress");
    ipPrefixLen @7 :UInt32 $Json.name("IPPrefixLen");
    ipv6Gateway @8 :Text $Json.name("IPv6Gateway");
    globalIpv6Address @9 :Text $Json.name("GlobalIPv6Address");
    globalIpv6PrefixLen @10 :UInt32 $Json.name("GlobalIPv6PrefixLen");
    macAddress @11 :Text $Json.name("MacAddress");
    driverOpts @12 :Json.Value $Json.name("DriverOpts");
  }


  struct ContainerCreateRequest {
    # Request body for ContainerCreate operation - flattened structure
    # Container configuration fields
    hostname @0 :Text $Json.name("Hostname");
    domainname @1 :Text $Json.name("Domainname");
    user @2 :Text $Json.name("User");
    attachStdin @3 :Bool = false $Json.name("AttachStdin");
    attachStdout @4 :Bool = false $Json.name("AttachStdout");
    attachStderr @5 :Bool = false $Json.name("AttachStderr");
    exposedPorts @6 :Json.Value $Json.name("ExposedPorts"); # Ports mapped to empty objects
    tty @7 :Bool $Json.name("Tty");
    openStdin @8 :Bool $Json.name("OpenStdin");
    stdinOnce @9 :Bool $Json.name("StdinOnce");
    env @10 :List(Text) $Json.name("Env"); # Environment variables in "KEY=value" format
    cmd @11 :List(Text) $Json.name("Cmd"); # Command to run
    entrypoint @12 :Text $Json.name("Entrypoint"); # Can be string or array - simplified as Text
    image @13 :Text $Json.name("Image"); # Image name/reference
    labels @14 :Json.Value $Json.name("Labels"); # Labels as key-value pairs
    volumes @15 :Json.Value $Json.name("Volumes"); # Volume mount points mapped to empty objects
    workingDir @16 :Text $Json.name("WorkingDir");
    networkDisabled @17 :Bool $Json.name("NetworkDisabled");
    macAddress @18 :Text $Json.name("MacAddress");
    stopSignal @19 :Text $Json.name("StopSignal");
    stopTimeout @20 :UInt32 = 10 $Json.name("StopTimeout");

    # Host configuration
    hostConfig @21 :HostConfig $Json.name("HostConfig");

    # Networking configuration
    # networkingConfig @22 :NetworkingConfig $Json.name("NetworkingConfig");

    struct HostConfig {
      # Container configuration that depends on the host
      binds @0 :List(Text) $Json.name("Binds"); # Volume bindings
      links @1 :List(Text) $Json.name("Links");
      memory @2 :UInt32 $Json.name("Memory");
      memorySwap @3 :UInt32 $Json.name("MemorySwap");
      memoryReservation @4 :UInt32 $Json.name("MemoryReservation");
      kernelMemory @5 :UInt32 $Json.name("KernelMemory");
      nanoCpus @6 :UInt32 $Json.name("NanoCpus");
      cpuPercent @7 :UInt32 $Json.name("CpuPercent");
      cpuShares @8 :UInt32 $Json.name("CpuShares");
      cpuPeriod @9 :UInt32 $Json.name("CpuPeriod");
      cpuRealtimePeriod @10 :UInt32 $Json.name("CpuRealtimePeriod");
      cpuRealtimeRuntime @11 :UInt32 $Json.name("CpuRealtimeRuntime");
      cpuQuota @12 :UInt32 $Json.name("CpuQuota");
      cpusetCpus @13 :Text $Json.name("CpusetCpus");
      cpusetMems @14 :Text $Json.name("CpusetMems");
      maximumIOps @15 :UInt32 $Json.name("MaximumIOps");
      maximumIOBps @16 :UInt32 $Json.name("MaximumIOBps");
      blkioWeight @17 :UInt16 $Json.name("BlkioWeight");
      blkioWeightDevice @18 :List(WeightDevice) $Json.name("BlkioWeightDevice");
      blkioDeviceReadBps @19 :List(ThrottleDevice) $Json.name("BlkioDeviceReadBps");
      blkioDeviceReadIOps @20 :List(ThrottleDevice) $Json.name("BlkioDeviceReadIOps");
      blkioDeviceWriteBps @21 :List(ThrottleDevice) $Json.name("BlkioDeviceWriteBps");
      blkioDeviceWriteIOps @22 :List(ThrottleDevice) $Json.name("BlkioDeviceWriteIOps");
      memorySwappiness @23 :UInt32 $Json.name("MemorySwappiness");
      oomKillDisable @24 :Bool $Json.name("OomKillDisable");
      oomScoreAdj @25 :Int32 $Json.name("OomScoreAdj");
      pidMode @26 :Text $Json.name("PidMode");
      pidsLimit @27 :Int32 $Json.name("PidsLimit"); # Can be -1
      portBindings @28 :Json.Value $Json.name("PortBindings"); # Port bindings map
      publishAllPorts @29 :Bool $Json.name("PublishAllPorts");
      privileged @30 :Bool $Json.name("Privileged");
      readonlyRootfs @31 :Bool $Json.name("ReadonlyRootfs");
      dns @32 :List(Text) $Json.name("Dns");
      dnsOptions @33 :List(Text) $Json.name("DnsOptions");
      dnsSearch @34 :List(Text) $Json.name("DnsSearch");
      volumesFrom @35 :List(Text) $Json.name("VolumesFrom");
      capAdd @36 :List(Text) $Json.name("CapAdd");
      capDrop @37 :List(Text) $Json.name("CapDrop");
      groupAdd @38 :List(Text) $Json.name("GroupAdd");
      restartPolicy @39 :RestartPolicy $Json.name("RestartPolicy");
      autoRemove @40 :Bool $Json.name("AutoRemove");
      networkMode @41 :Text $Json.name("NetworkMode");
      devices @42 :List(DeviceMapping) $Json.name("Devices");
      ulimits @43 :List(Ulimit) $Json.name("Ulimits");
      logConfig @44 :LogConfig $Json.name("LogConfig");
      securityOpt @45 :List(Text) $Json.name("SecurityOpt");
      storageOpt @46 :Json.Value $Json.name("StorageOpt");
      cgroupParent @47 :Text $Json.name("CgroupParent");
      volumeDriver @48 :Text $Json.name("VolumeDriver");
      shmSize @49 :UInt32 $Json.name("ShmSize");
      extraHosts @50 :List(Text) $Json.name("ExtraHosts"); # --add-host entries in "host:ip" format

    }
  }

  struct ContainerCreateResponse {
    # Response from ContainerCreate operation
    id @0 :Text $Json.name("Id"); # The ID of the created container
    warnings @1 :List(Text) $Json.name("Warnings"); # Warnings encountered when creating the container
  }

  struct ContainerMonitorResponse {
    statusCode @0 :Int32 $Json.name("StatusCode");
  }

  struct ContainerState {
    # Container's running state
    status @0 :Text $Json.name("Status"); # "created", "running", "paused", "restarting", "removing", "exited", "dead"
    running @1 :Bool $Json.name("Running");
    paused @2 :Bool $Json.name("Paused");
    restarting @3 :Bool $Json.name("Restarting");
    oomKilled @4 :Bool $Json.name("OOMKilled");
    dead @5 :Bool $Json.name("Dead");
    pid @6 :UInt32 $Json.name("Pid");
    exitCode @7 :Int32 $Json.name("ExitCode");
    error @8 :Text $Json.name("Error");
    startedAt @9 :Text $Json.name("StartedAt");
    finishedAt @10 :Text $Json.name("FinishedAt");
  }

  struct GraphDriverData {
    # Information about the storage driver
    name @0 :Text $Json.name("Name");
    data @1 :Json.Value $Json.name("Data");
  }

  struct MountPoint {
    # Mount point configuration inside the container
    type @0 :Text $Json.name("Type"); # "bind", "volume", "tmpfs", "npipe"
    name @1 :Text $Json.name("Name");
    source @2 :Text $Json.name("Source");
    destination @3 :Text $Json.name("Destination");
    driver @4 :Text $Json.name("Driver");
    mode @5 :Text $Json.name("Mode");
    rw @6 :Bool $Json.name("RW");
    propagation @7 :Text $Json.name("Propagation");
  }

  struct NetworkSettings {
    # Network settings for the container
    bridge @0 :Text $Json.name("Bridge");
    sandboxId @1 :Text $Json.name("SandboxID");
    hairpinMode @2 :Bool $Json.name("HairpinMode");
    linkLocalIpv6Address @3 :Text $Json.name("LinkLocalIPv6Address");
    linkLocalIpv6PrefixLen @4 :UInt32 $Json.name("LinkLocalIPv6PrefixLen");
    sandboxKey @5 :Text $Json.name("SandboxKey");
    endpointId @6 :Text $Json.name("EndpointID");
    gateway @7 :Text $Json.name("Gateway");
    globalIpv6Address @8 :Text $Json.name("GlobalIPv6Address");
    globalIpv6PrefixLen @9 :UInt32 $Json.name("GlobalIPv6PrefixLen");
    ipAddress @10 :Text $Json.name("IPAddress");
    ipPrefixLen @11 :UInt32 $Json.name("IPPrefixLen");
    ipv6Gateway @12 :Text $Json.name("IPv6Gateway");
    macAddress @13 :Text $Json.name("MacAddress");
    networks @14 :Json.Value $Json.name("Networks");
    ports @15 :Json.Value $Json.name("Ports");
  }

  struct ContainerConfig {
    # Container configuration (reusable from create)
    hostname @0 :Text $Json.name("Hostname");
    domainname @1 :Text $Json.name("Domainname");
    user @2 :Text $Json.name("User");
    attachStdin @3 :Bool $Json.name("AttachStdin");
    attachStdout @4 :Bool $Json.name("AttachStdout");
    attachStderr @5 :Bool $Json.name("AttachStderr");
    exposedPorts @6 :Json.Value $Json.name("ExposedPorts");
    tty @7 :Bool $Json.name("Tty");
    openStdin @8 :Bool $Json.name("OpenStdin");
    stdinOnce @9 :Bool $Json.name("StdinOnce");
    env @10 :List(Text) $Json.name("Env");
    cmd @11 :List(Text) $Json.name("Cmd");
    image @12 :Text $Json.name("Image");
    volumes @13 :Json.Value $Json.name("Volumes");
    workingDir @14 :Text $Json.name("WorkingDir");
    entrypoint @15 :List(Text) $Json.name("Entrypoint");
    networkDisabled @16 :Bool $Json.name("NetworkDisabled");
    macAddress @17 :Text $Json.name("MacAddress");
    onBuild @18 :List(Text) $Json.name("OnBuild");
    labels @19 :Json.Value $Json.name("Labels");
    stopSignal @20 :Text $Json.name("StopSignal");
    stopTimeout @21 :UInt32 $Json.name("StopTimeout");
    shell @22 :List(Text) $Json.name("Shell");
  }

  struct ContainerInspectResponse {
    # Response from ContainerInspect operation
    id @0 :Text $Json.name("Id");
    created @1 :Text $Json.name("Created");
    path @2 :Text $Json.name("Path");
    args @3 :List(Text) $Json.name("Args");
    state @4 :ContainerState $Json.name("State");
    networkSettings @5 :NetworkSettings $Json.name("NetworkSettings");
  }

  struct Command {
    struct ContainerCreate {
      struct Params {
        name @0 :Text; # Optional container name
        body @1 :ContainerCreateRequest;
      }
      struct Result {
        response @0 :ContainerCreateResponse;
      }
    }

    struct ContainerInspect {
      struct Params {
        id @0 :Text; # Container ID or name
      }
      struct Result {
        response @0 :ContainerInspectResponse;
      }
    }
  }

  # Network inspection response (GET /networks/{id})
  struct NetworkInspectResponse {
    name @0 :Text $Json.name("Name");
    id @1 :Text $Json.name("Id");
    ipam @2 :IPAM $Json.name("IPAM");

    struct IPAM {
      driver @0 :Text $Json.name("Driver");
      config @1 :List(IPAMConfig) $Json.name("Config");

      struct IPAMConfig {
        subnet @0 :Text $Json.name("Subnet");
        gateway @1 :Text $Json.name("Gateway");
      }
    }
  }

  # Network create request (POST /networks/create)
  # Equivalent to: docker network create -d bridge --ipv6 workerd-network
  struct NetworkCreateRequest {
    name @0 :Text $Json.name("Name");
    driver @1 :Text $Json.name("Driver");  # "bridge", "overlay", etc.
    enableIpv6 @2 :Bool $Json.name("EnableIPv6");
  }

  # Network create response
  struct NetworkCreateResponse {
    id @0 :Text $Json.name("Id");
    warning @1 :Text $Json.name("Warning");
  }
}
