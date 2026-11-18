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

#include <kj/async-io.h>
#include <kj/test.h>

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

// The UDP flow tests below run a fake proxy-everything sidecar on a loopback listener.

constexpr kj::StringPtr CONNECT_ACCEPTED = "HTTP/1.1 200 OK\r\n\r\n"_kj;

struct DownEvent {
  kj::StringPtr kind;
  kj::String payload;
};

struct DownRecording {
  kj::Vector<DownEvent> events;
  size_t consumed = 0;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> waiter;

  void add(kj::StringPtr kind, kj::String payload = kj::str()) {
    events.add(DownEvent{kind, kj::mv(payload)});

    KJ_IF_SOME(fulfiller, waiter) {
      fulfiller->fulfill();
      waiter = kj::none;
    }
  }

  DownEvent next(kj::WaitScope& waitScope) {
    while (consumed == events.size()) {
      auto paf = kj::newPromiseAndFulfiller<void>();
      waiter = kj::mv(paf.fulfiller);
      paf.promise.wait(waitScope);
    }

    return kj::mv(events[consumed++]);
  }
};

class RecordingDatagramStream final: public rpc::DatagramStream::Server {
 public:
  explicit RecordingDatagramStream(DownRecording& recording): recording(recording) {}

 private:
  DownRecording& recording;

  kj::Promise<void> send(SendContext context) override {
    recording.add("datagram"_kj, kj::heapString(context.getParams().getDatagram().asChars()));
    return kj::READY_NOW;
  }

  kj::Promise<void> end(EndContext) override {
    recording.add("end"_kj);
    return kj::READY_NOW;
  }

  kj::Promise<void> abort(AbortContext) override {
    recording.add("abort"_kj);
    return kj::READY_NOW;
  }
};

struct UdpFlowTest {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  kj::WaitScope& waitScope = io.waitScope;
  kj::Network& network = io.provider->getNetwork();
  kj::Own<kj::ConnectionReceiver> sidecar =
      network.parseAddress("127.0.0.1", 0).wait(waitScope)->listen();
  kj::Maybe<uint16_t> ingressHostPort = static_cast<uint16_t>(sidecar->getPort());
  DownRecording down;
  rpc::Container::UdpPort::Client port = newDockerUdpPort(network, ingressHostPort, 5353);

  capnp::RemotePromise<rpc::Container::UdpPort::ConnectResults> connect() {
    auto request = port.connectRequest();
    request.setDown(kj::heap<RecordingDatagramStream>(down));
    return request.send();
  }

  // Accepts the tunnel and checks the request head.
  kj::Own<kj::AsyncIoStream> accept() {
    auto conn = sidecar->accept().wait(waitScope);
    kj::Vector<char> head;

    while (head.size() < 4 || memcmp(head.end() - 4, "\r\n\r\n", 4) != 0) {
      char next;
      KJ_REQUIRE(conn->tryRead(&next, 1, 1).wait(waitScope) == 1);
      head.add(next);
    }

    auto text = kj::heapString(head.asPtr());
    KJ_EXPECT(text.startsWith("CONNECT 127.0.0.1:5353 HTTP/1.1\r\n"), text);
    KJ_EXPECT(text.contains("\r\nX-Dst-Addr: 127.0.0.1:5353\r\n"), text);
    KJ_EXPECT(text.contains("\r\nX-Dst-Protocol: udp\r\n"), text);
    return conn;
  }

  // Reads one frame and returns its datagram.
  kj::String readDatagram(kj::AsyncIoStream& conn) {
    kj::byte length[2];
    KJ_REQUIRE(conn.tryRead(length, 2, 2).wait(waitScope) == 2);
    auto datagram = kj::heapArray<char>(size_t(length[0]) << 8 | length[1]);
    auto size = conn.tryRead(datagram.begin(), datagram.size(), datagram.size()).wait(waitScope);
    KJ_REQUIRE(size == datagram.size());
    return kj::heapString(datagram);
  }
};

void appendLength(kj::Vector<kj::byte>& out, uint16_t length) {
  out.add(static_cast<kj::byte>(length >> 8));
  out.add(static_cast<kj::byte>(length));
}

void appendDatagram(kj::Vector<kj::byte>& out, kj::StringPtr payload) {
  appendLength(out, static_cast<uint16_t>(payload.size()));
  out.addAll(payload.asBytes());
}

kj::Promise<void> sendUp(rpc::DatagramStream::Client& up, kj::StringPtr payload) {
  auto request = up.sendRequest();
  request.setDatagram(payload.asBytes());
  return request.send();
}

KJ_TEST("UDP flow sends early datagrams after the request head and round trips payloads") {
  UdpFlowTest test;
  auto connect = test.connect();
  auto up = connect.getUp();
  auto largest = kj::str(kj::repeat('b', 1500));

  kj::Vector<kj::Promise<void>> sends;
  sends.add(sendUp(up, ""_kj));
  sends.add(sendUp(up, "x"_kj));
  sends.add(sendUp(up, largest));
  sends.add(sendUp(up, kj::str(kj::repeat('c', 1501))));
  sends.add(sendUp(up, "z"_kj));

  auto conn = test.accept();
  KJ_EXPECT(test.readDatagram(*conn) == ""_kj);
  KJ_EXPECT(test.readDatagram(*conn) == "x"_kj);
  KJ_EXPECT(test.readDatagram(*conn) == largest);
  // The oversized datagram is dropped.
  KJ_EXPECT(test.readDatagram(*conn) == "z"_kj);

  kj::Vector<kj::byte> reply;
  reply.addAll(CONNECT_ACCEPTED.asBytes());
  appendDatagram(reply, ""_kj);
  appendDatagram(reply, "x"_kj);
  appendDatagram(reply, largest);
  conn->write(reply.asPtr()).wait(test.waitScope);

  connect.wait(test.waitScope);
  kj::joinPromises(sends.releaseAsArray()).wait(test.waitScope);

  for (kj::StringPtr expected: {""_kj, "x"_kj, largest.asPtr()}) {
    auto event = test.down.next(test.waitScope);
    KJ_EXPECT(event.kind == "datagram"_kj);
    KJ_EXPECT(event.payload == expected);
  }

  sendUp(up, "after"_kj).wait(test.waitScope);
  KJ_EXPECT(test.readDatagram(*conn) == "after"_kj);
}

KJ_TEST("UDP flow fails connect with the sidecar's status when it refuses the flow") {
  UdpFlowTest test;
  auto connect = test.connect();
  auto up = connect.getUp();
  auto early = sendUp(up, "early"_kj).eagerlyEvaluate([](kj::Exception&&) {});
  auto conn = test.accept();
  KJ_EXPECT(test.readDatagram(*conn) == "early"_kj);

  // The sidecar closes the tunnel after the status. The early datagram was read first, so the
  // close is a FIN and does not reset the status away.
  conn->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n\r\nbad target."_kj.asBytes())
      .wait(test.waitScope);
  conn = nullptr;

  KJ_EXPECT_THROW_MESSAGE(
      "Connecting to container UDP port through proxy-everything failed: [400] Bad Request bad "
      "target.",
      connect.wait(test.waitScope));
}

KJ_TEST("UDP flow decodes frames split across reads") {
  UdpFlowTest test;
  auto connect = test.connect();
  auto conn = test.accept();

  // Each write ends inside a frame and waits until the datagram before the cut arrives, so the
  // flow reads the cut separately: first between the two length bytes, then inside a datagram.
  kj::Vector<kj::byte> frames;
  appendDatagram(frames, "a"_kj);
  appendDatagram(frames, "bb"_kj);
  auto betweenLengthBytes = frames.size() - 3;
  appendDatagram(frames, "ccc"_kj);
  appendDatagram(frames, "dddd"_kj);
  auto insideDatagram = frames.size() - 2;

  kj::Vector<kj::byte> reply;
  reply.addAll(CONNECT_ACCEPTED.asBytes());
  reply.addAll(frames.asPtr().first(betweenLengthBytes));
  conn->write(reply.asPtr()).wait(test.waitScope);
  connect.wait(test.waitScope);
  KJ_EXPECT(test.down.next(test.waitScope).payload == "a"_kj);

  conn->write(frames.asPtr().slice(betweenLengthBytes, insideDatagram)).wait(test.waitScope);
  KJ_EXPECT(test.down.next(test.waitScope).payload == "bb"_kj);
  KJ_EXPECT(test.down.next(test.waitScope).payload == "ccc"_kj);

  conn->write(frames.asPtr().slice(insideDatagram, frames.size())).wait(test.waitScope);
  auto last = test.down.next(test.waitScope);
  KJ_EXPECT(last.kind == "datagram"_kj);
  KJ_EXPECT(last.payload == "dddd"_kj);
}

KJ_TEST("UDP flow ends down and closes the tunnel on FLOW_END") {
  UdpFlowTest test;
  auto connect = test.connect();
  auto conn = test.accept();

  kj::Vector<kj::byte> reply;
  reply.addAll(CONNECT_ACCEPTED.asBytes());
  appendDatagram(reply, "last"_kj);
  appendLength(reply, 0xffff);
  reply.add(0);
  conn->write(reply.asPtr()).wait(test.waitScope);
  connect.wait(test.waitScope);

  KJ_EXPECT(test.down.next(test.waitScope).payload == "last"_kj);
  KJ_EXPECT(test.down.next(test.waitScope).kind == "end"_kj);

  kj::byte unused;
  KJ_EXPECT(conn->tryRead(&unused, 1, 1).wait(test.waitScope) == 0);
}

KJ_TEST("UDP flow skips frames above 1500 bytes and stays open") {
  // A 0xFFFE-byte frame is larger than the flow's read buffer, so skipping it spans reads.
  for (uint16_t length: {1501, 0xfffe}) {
    UdpFlowTest test;
    auto connect = test.connect();
    auto up = connect.getUp();
    auto conn = test.accept();

    kj::Vector<kj::byte> reply;
    reply.addAll(CONNECT_ACCEPTED.asBytes());
    appendLength(reply, length);
    auto oversized = kj::heapArray<kj::byte>(length);
    oversized.asPtr().fill('x');
    reply.addAll(oversized);
    appendDatagram(reply, "next"_kj);

    // Written in pieces, so the oversized frame also arrives over several writes.
    for (size_t offset = 0; offset < reply.size(); offset += 10000) {
      auto end = kj::min(offset + 10000, reply.size());
      conn->write(reply.asPtr().slice(offset, end)).wait(test.waitScope);
    }

    connect.wait(test.waitScope);

    auto datagram = test.down.next(test.waitScope);
    KJ_EXPECT(datagram.kind == "datagram"_kj, length);
    KJ_EXPECT(datagram.payload == "next"_kj, length);

    sendUp(up, "still open"_kj).wait(test.waitScope);
    KJ_EXPECT(test.readDatagram(*conn) == "still open"_kj, length);
    KJ_EXPECT(test.down.events.size() == 1, length);
  }
}

KJ_TEST("UDP flow keeps receiving after end() and ends down when the sidecar closes") {
  UdpFlowTest test;
  auto connect = test.connect();
  auto up = connect.getUp();
  auto conn = test.accept();

  conn->write(CONNECT_ACCEPTED.asBytes()).wait(test.waitScope);
  connect.wait(test.waitScope);

  up.endRequest().send().wait(test.waitScope);
  KJ_EXPECT_THROW_MESSAGE("send() after end()", sendUp(up, "late"_kj).wait(test.waitScope));

  kj::Vector<kj::byte> reply;
  appendDatagram(reply, "still flowing"_kj);
  conn->write(reply.asPtr()).wait(test.waitScope);

  auto datagram = test.down.next(test.waitScope);
  KJ_EXPECT(datagram.kind == "datagram"_kj);
  KJ_EXPECT(datagram.payload == "still flowing"_kj);

  conn->shutdownWrite();
  KJ_EXPECT(test.down.next(test.waitScope).kind == "end"_kj);
}

KJ_TEST("UDP flow closes the tunnel when up is released") {
  UdpFlowTest test;
  kj::Own<kj::AsyncIoStream> conn;

  {
    auto connect = test.connect();
    conn = test.accept();
    conn->write(CONNECT_ACCEPTED.asBytes()).wait(test.waitScope);
    connect.wait(test.waitScope);
  }

  kj::byte unused;
  KJ_EXPECT(conn->tryRead(&unused, 1, 1).wait(test.waitScope) == 0);
  KJ_EXPECT(test.down.events.size() == 0);
}

}  // namespace
}  // namespace workerd::server
