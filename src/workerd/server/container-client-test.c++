// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for container-client JSON decoding.
//
// These tests verify that JSON responses from the Docker API are decoded into
// Cap'n Proto messages whose backing storage outlives the decode call. The
// original decodeJsonResponse<T>() returned a Builder pointing into a
// stack-local MallocMessageBuilder; the builder was then accessed after the
// message was destroyed (use-after-free). Under ASAN this test would crash
// with the old code.

#include "container-client.h"

#include "channel-token.h"

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>
#include <kj/test.h>
#include <kj/timer.h>

namespace workerd::server {
namespace {

// Regression test for VULN-127728: ContainerInspectResponse decode must not
// use-after-free.  With the old buggy code every field access after decode
// dereferences freed heap memory (detectable by ASAN).
KJ_TEST("decodeJsonResponse ContainerInspectResponse - no use-after-free") {
  // Minimal JSON matching what Docker returns for /containers/<id>/json.
  auto json = R"({
    "Id": "abc123",
    "Created": "2025-01-01T00:00:00Z",
    "Path": "/bin/sh",
    "Args": ["--http-egress-port", "9000"],
    "State": {
      "Status": "running",
      "Running": true,
      "Paused": false,
      "Restarting": false,
      "OOMKilled": false,
      "Dead": false,
      "Pid": 42,
      "ExitCode": 0,
      "Error": "",
      "StartedAt": "2025-01-01T00:00:01Z",
      "FinishedAt": "0001-01-01T00:00:00Z"
    },
    "NetworkSettings": {
      "Bridge": "",
      "SandboxID": "",
      "HairpinMode": false,
      "LinkLocalIPv6Address": "",
      "LinkLocalIPv6PrefixLen": 0,
      "SandboxKey": "",
      "EndpointID": "",
      "Gateway": "172.17.0.1",
      "GlobalIPv6Address": "",
      "GlobalIPv6PrefixLen": 0,
      "IPAddress": "172.17.0.2",
      "IPPrefixLen": 16,
      "IPv6Gateway": "",
      "MacAddress": "02:42:ac:11:00:02",
      "Networks": {},
      "Ports": {
        "8080/tcp": [
          {"HostIp": "0.0.0.0", "HostPort": "55000"}
        ]
      }
    }
  })"_kj;

  auto message = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(json);
  auto root = message->getRoot<docker_api::Docker::ContainerInspectResponse>();

  // Every access below would be a heap-use-after-free with the old code.
  KJ_EXPECT(root.getId() == "abc123");
  KJ_EXPECT(root.hasState());

  auto state = root.getState();
  KJ_EXPECT(state.getStatus() == "running");
  KJ_EXPECT(state.getRunning() == true);

  KJ_EXPECT(root.hasArgs());
  auto args = root.getArgs();
  KJ_EXPECT(args.size() == 2);
  KJ_EXPECT(args[0] == "--http-egress-port");
  KJ_EXPECT(args[1] == "9000");

  auto ports = root.getNetworkSettings().getPorts().getObject();
  KJ_EXPECT(ports.size() == 1);
  KJ_EXPECT(ports[0].getName() == "8080/tcp");
  auto array = ports[0].getValue().getArray();
  KJ_EXPECT(array.size() == 1);
  auto obj = array[0].getObject();
  KJ_EXPECT(obj.size() == 2);
  // HostPort is obj[1] in the order Docker returns
  auto mappedPort = obj[1].getValue().getString();
  KJ_EXPECT(mappedPort == "55000");
}

KJ_TEST("decodeJsonResponse NetworkInspectResponse - no use-after-free") {
  auto json = R"({
    "Name": "bridge",
    "Id": "net123",
    "IPAM": {
      "Driver": "default",
      "Config": [
        {"Subnet": "172.17.0.0/16", "Gateway": "172.17.0.1"}
      ]
    }
  })"_kj;

  auto message = decodeJsonResponse<docker_api::Docker::NetworkInspectResponse>(json);
  auto root = message->getRoot<docker_api::Docker::NetworkInspectResponse>();

  KJ_EXPECT(root.getName() == "bridge");
  auto ipamConfig = root.getIpam().getConfig();
  KJ_EXPECT(ipamConfig.size() == 1);
  KJ_EXPECT(ipamConfig[0].getSubnet() == "172.17.0.0/16");
  KJ_EXPECT(ipamConfig[0].getGateway() == "172.17.0.1");
}

KJ_TEST("decodeJsonResponse ContainerMonitorResponse - no use-after-free") {
  auto json = R"({"StatusCode": 0})"_kj;

  auto message = decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(json);
  auto root = message->getRoot<docker_api::Docker::ContainerMonitorResponse>();

  KJ_EXPECT(root.getStatusCode() == 0);
}

KJ_TEST("decodeJsonResponse ContainerMonitorResponse - non-zero exit") {
  auto json = R"({"StatusCode": 137})"_kj;

  auto message = decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(json);
  auto root = message->getRoot<docker_api::Docker::ContainerMonitorResponse>();

  KJ_EXPECT(root.getStatusCode() == 137);
}

KJ_TEST("ContainerCreateRequest encodes structured mounts with NoCopy") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();

  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  root.setImage("test-image");

  auto mounts = root.initHostConfig().initMounts(1);
  auto mount = mounts[0];
  mount.setType("volume");
  mount.setSource("snapshot-clone-volume");
  mount.setTarget("/app/data");
  mount.initVolumeOptions().setNoCopy(true);

  auto json = codec.encode(root);
  auto jsonText = json.asPtr();

  KJ_EXPECT(jsonText.contains("\"Mounts\""));
  KJ_EXPECT(jsonText.contains("\"VolumeOptions\""));
  KJ_EXPECT(jsonText.contains("\"NoCopy\":true"));

  auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
  auto decodedRoot = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>();
  auto decodedMounts = decodedRoot.getHostConfig().getMounts();

  KJ_REQUIRE(decodedMounts.size() == 1);
  KJ_EXPECT(decodedMounts[0].getType() == "volume");
  KJ_EXPECT(decodedMounts[0].getSource() == "snapshot-clone-volume");
  KJ_EXPECT(decodedMounts[0].getTarget() == "/app/data");
  KJ_EXPECT(decodedMounts[0].getVolumeOptions().getNoCopy());
}

KJ_TEST("ContainerCreateRequest encodes Entrypoint as array") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();

  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  root.setImage("test-image");

  auto entrypoint = root.initEntrypoint(1);
  entrypoint.set(0, "/bin/cp");

  auto json = codec.encode(root);
  auto jsonText = json.asPtr();

  KJ_EXPECT(jsonText.contains("\"Entrypoint\":[\"/bin/cp\"]"));

  auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
  auto decodedRoot = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>();
  auto decodedEntrypoint = decodedRoot.getEntrypoint();

  KJ_REQUIRE(decodedEntrypoint.size() == 1);
  KJ_EXPECT(decodedEntrypoint[0] == "/bin/cp");
}

KJ_TEST("ContainerCreateRequest encodes HostConfig Dns") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();

  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  root.setImage("test-image");

  auto dns = root.initHostConfig().initDns(2);
  dns.set(0, "1.1.1.1");
  dns.set(1, "8.8.8.8");

  auto json = codec.encode(root);
  auto jsonText = json.asPtr();

  KJ_EXPECT(jsonText.contains("\"Dns\""));
  KJ_EXPECT(jsonText.contains("1.1.1.1"));
  KJ_EXPECT(jsonText.contains("8.8.8.8"));

  auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
  auto decodedRoot = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>();
  auto decodedDns = decodedRoot.getHostConfig().getDns();

  KJ_REQUIRE(decodedDns.size() == 2);
  KJ_EXPECT(decodedDns[0] == "1.1.1.1");
  KJ_EXPECT(decodedDns[1] == "8.8.8.8");
}

KJ_TEST("ContainerCreateRequest encodes configured container privileges") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();

  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  root.setImage("test-image");

  auto hostConfig = root.initHostConfig();
  ContainerPrivileges privileges{
    .capabilities = kj::arr(kj::str("SYS_ADMIN")),
    .devices = kj::arr(ContainerPrivileges::Device{
      .pathOnHost = kj::str("/dev/fuse"),
      .pathInContainer = kj::str("/dev/fuse"),
      .cgroupPermissions = kj::str("rwm"),
    }),
    .securityOpt = kj::arr(kj::str("apparmor:unconfined")),
  };
  configureContainerPrivileges(hostConfig, privileges);

  auto json = codec.encode(root);
  auto jsonText = json.asPtr();

  KJ_EXPECT(jsonText.contains("\"CapAdd\""));
  KJ_EXPECT(jsonText.contains("\"SYS_ADMIN\""));
  KJ_EXPECT(jsonText.contains("\"Devices\""));
  KJ_EXPECT(jsonText.contains("\"PathOnHost\":\"/dev/fuse\""));
  KJ_EXPECT(jsonText.contains("\"PathInContainer\":\"/dev/fuse\""));
  KJ_EXPECT(jsonText.contains("\"CgroupPermissions\":\"rwm\""));
  KJ_EXPECT(jsonText.contains("\"SecurityOpt\""));
  KJ_EXPECT(jsonText.contains("\"apparmor:unconfined\""));

  auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
  auto decodedRoot = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>();
  auto decodedHostConfig = decodedRoot.getHostConfig();
  auto decodedCapAdd = decodedHostConfig.getCapAdd();
  auto decodedDevices = decodedHostConfig.getDevices();
  auto decodedSecurityOpt = decodedHostConfig.getSecurityOpt();

  KJ_REQUIRE(decodedCapAdd.size() == 1);
  KJ_EXPECT(decodedCapAdd[0] == "SYS_ADMIN");

  KJ_REQUIRE(decodedDevices.size() == 1);
  KJ_EXPECT(decodedDevices[0].getPathOnHost() == "/dev/fuse");
  KJ_EXPECT(decodedDevices[0].getPathInContainer() == "/dev/fuse");
  KJ_EXPECT(decodedDevices[0].getCgroupPermissions() == "rwm");

  KJ_REQUIRE(decodedSecurityOpt.size() == 1);
  KJ_EXPECT(decodedSecurityOpt[0] == "apparmor:unconfined");
}

KJ_TEST("ContainerCreateRequest omits empty container privileges") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();

  capnp::MallocMessageBuilder message;
  auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  root.setImage("test-image");

  auto hostConfig = root.initHostConfig();
  ContainerPrivileges privileges;
  configureContainerPrivileges(hostConfig, privileges);

  auto json = codec.encode(root);
  auto jsonText = json.asPtr();

  KJ_EXPECT(!jsonText.contains("\"CapAdd\""));
  KJ_EXPECT(!jsonText.contains("\"SYS_ADMIN\""));
  KJ_EXPECT(!jsonText.contains("\"Devices\""));
  KJ_EXPECT(!jsonText.contains("\"PathOnHost\""));
  KJ_EXPECT(!jsonText.contains("\"PathInContainer\""));
  KJ_EXPECT(!jsonText.contains("\"CgroupPermissions\""));
  KJ_EXPECT(!jsonText.contains("\"SecurityOpt\""));
  KJ_EXPECT(!jsonText.contains("\"apparmor:unconfined\""));

  auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
  auto decodedRoot = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>();
  auto decodedHostConfig = decodedRoot.getHostConfig();

  KJ_EXPECT(decodedHostConfig.getCapAdd().size() == 0);
  KJ_EXPECT(decodedHostConfig.getDevices().size() == 0);
  KJ_EXPECT(decodedHostConfig.getSecurityOpt().size() == 0);
}

// ======================================================================================
// DockerPort::connect() use-after-free regression test.
//
// DockerPort dereferences its ContainerClient (network, byteStreamFactory) AFTER several co_await
// points in connect(). If it holds only a bare `ContainerClient&`, then the safety of those derefs
// depends entirely on nothing dropping the last ContainerClient reference while a connect() is
// suspended. Nothing in the type system enforces that coupling. The sibling DockerProcessHandle
// avoids the hazard by holding `containerClient.addRef()`; DockerPort should do the same.
//
// This test drives connect() to a suspension point (parseAddress), drops the only strong reference
// to the ContainerClient, then resumes connect(). With a bare reference this reads freed memory and
// AddressSanitizer aborts; with the addRef fix DockerPort keeps the ContainerClient alive and the
// test passes.

// A NetworkAddress whose connect() hands back a pre-supplied stream (the proxy connection).
class MockAddress final: public kj::NetworkAddress {
 public:
  explicit MockAddress(kj::Own<kj::AsyncIoStream> streamParam): stream(kj::mv(streamParam)) {}
  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
    return kj::mv(KJ_ASSERT_NONNULL(stream));
  }
  kj::Own<kj::ConnectionReceiver> listen() override {
    KJ_UNIMPLEMENTED("listen");
  }
  kj::Own<kj::NetworkAddress> clone() override {
    KJ_UNIMPLEMENTED("clone");
  }
  kj::String toString() override {
    return kj::str("mock");
  }

 private:
  kj::Maybe<kj::Own<kj::AsyncIoStream>> stream;
};

// A Network whose parseAddress() suspends (returns a fulfiller-controlled promise) so the test can
// free the ContainerClient while connect() is parked at its first co_await.
class SuspendNetwork final: public kj::Network {
 public:
  kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(kj::StringPtr addr, uint = 0) override {
    // Only the connect() path parses the 127.0.0.1 ingress-proxy address. The ctor's stale-snapshot
    // check and ~ContainerClient's docker-socket cleanup also call parseAddress; leave those pending
    // so they don't disturb the connect() suspension the test is controlling.
    if (addr.contains("127.0.0.1"_kjc)) {
      auto paf = kj::newPromiseAndFulfiller<kj::Own<kj::NetworkAddress>>();
      fulfiller = kj::mv(paf.fulfiller);
      return kj::mv(paf.promise);
    }
    return kj::NEVER_DONE;
  }
  kj::Own<kj::NetworkAddress> getSockaddr(const void*, uint) override {
    KJ_UNIMPLEMENTED("getSockaddr");
  }
  kj::Own<kj::Network> restrictPeers(
      kj::ArrayPtr<const kj::StringPtr>, kj::ArrayPtr<const kj::StringPtr>) override {
    KJ_UNIMPLEMENTED("restrictPeers");
  }

  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Own<kj::NetworkAddress>>>> fulfiller;
};

class MockResolver final: public ChannelTokenHandler::Resolver {
 public:
  kj::Own<IoChannelFactory::SubrequestChannel> resolveEntrypoint(
      kj::StringPtr, kj::Maybe<kj::StringPtr>, Frankenvalue, Persistent) override {
    KJ_UNIMPLEMENTED("resolveEntrypoint");
  }
  kj::Own<IoChannelFactory::ActorClassChannel> resolveActorClass(
      kj::StringPtr, kj::Maybe<kj::StringPtr>, Frankenvalue, Persistent) override {
    KJ_UNIMPLEMENTED("resolveActorClass");
  }
  kj::Own<IoChannelFactory::ActorChannel> resolveActor(
      kj::StringPtr, kj::ArrayPtr<const byte>, kj::Maybe<kj::StringPtr>, Persistent) override {
    KJ_UNIMPLEMENTED("resolveActor");
  }
};

struct NullErrorHandler final: public kj::TaskSet::ErrorHandler {
  void taskFailed(kj::Exception&&) override {}
};

KJ_TEST("DockerPort connect() use-after-free on ContainerClient freed mid-call") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  capnp::ByteStreamFactory byteStreamFactory;
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  SuspendNetwork network;
  NullErrorHandler errorHandler;
  kj::TaskSet waitUntilTasks(errorHandler);
  MockResolver resolver;
  ChannelTokenHandler channelTokenHandler(resolver);

  auto container = kj::refcounted<ContainerClient>(byteStreamFactory, timer, network,
      kj::str("docker"), kj::str("test-container"), kj::str("test-image"), kj::str("interceptor"),
      waitUntilTasks, kj::Promise<void>(kj::READY_NOW), [](kj::Promise<void>) {}, channelTokenHandler,
      ContainerPrivileges{});
  container->setSidecarIngressHostPortForTest(8080);

  // Wrap the ContainerClient as a capnp Container capability. This Own is the only strong reference.
  rpc::Container::Client containerCap = kj::mv(container);

  // getTcpPort -> Port capability (resolves immediately; mutationQueue is READY_NOW).
  auto getPortReq = containerCap.getTcpPortRequest();
  getPortReq.setPort(1234);
  auto port = getPortReq.send().getPort();

  // Start connect(). It runs to `co_await network.parseAddress(...)` and suspends there.
  auto connectReq = port.connectRequest();
  auto connectPromise = connectReq.send();

  // Drive the loop until connect() has suspended at parseAddress.
  KJ_ASSERT(!connectPromise.poll(ws));
  KJ_ASSERT(network.fulfiller != kj::none, "connect() did not reach parseAddress");

  // Drop the only external strong ref to ContainerClient. Without the addRef fix, DockerPort holds a
  // bare reference, so nothing keeps ContainerClient alive across the suspended connect().
  { auto drop = kj::mv(containerCap); }

  // Resume connect(): supply a mock proxy connection, then a "200" so the HTTP CONNECT status
  // resolves and connect() proceeds to the containerClient.byteStreamFactory deref -- against freed
  // memory if the bare-reference bug is present.
  auto httpPipe = kj::newTwoWayPipe();
  auto serverEnd = kj::mv(httpPipe.ends[1]);
  auto respTask = serverEnd->write("HTTP/1.1 200 OK\r\n\r\n"_kjb).eagerlyEvaluate(nullptr);
  KJ_ASSERT_NONNULL(network.fulfiller)->fulfill(kj::heap<MockAddress>(kj::mv(httpPipe.ends[0])));

  // With the bug present, ASAN aborts here when connect() dereferences the freed ContainerClient.
  connectPromise.wait(ws);
}

}  // namespace
}  // namespace workerd::server
