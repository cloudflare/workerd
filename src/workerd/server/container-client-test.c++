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

}  // namespace
}  // namespace workerd::server
