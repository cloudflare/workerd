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

KJ_TEST("ContainerCreateRequest labels the container with its session and enable-internet") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();

  {
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
    root.setImage("test-image");
    setContainerLabels(root, kj::str("session-1"), true);

    auto json = codec.encode(root);
    auto jsonText = json.asPtr();
    KJ_EXPECT(jsonText.contains("\"dev.workerd.session\":\"session-1\""));
    KJ_EXPECT(jsonText.contains("\"dev.workerd.enable-internet\":\"true\""));

    // What Docker hands back in Config.Labels is read as what was written.
    auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
    auto labels = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>().getLabels();
    KJ_EXPECT(labels.getObject().size() == 2);
    auto enabled = readEnableInternetLabel(labels);
    KJ_EXPECT(KJ_ASSERT_NONNULL(enabled) == true);
  }

  {
    // No session: the enable-internet label alone.
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
    root.setImage("test-image");
    setContainerLabels(root, kj::none, false);

    auto json = codec.encode(root);
    auto jsonText = json.asPtr();
    KJ_EXPECT(!jsonText.contains("\"dev.workerd.session\""));
    KJ_EXPECT(jsonText.contains("\"dev.workerd.enable-internet\":\"false\""));

    auto decoded = decodeJsonResponse<docker_api::Docker::ContainerCreateRequest>(jsonText);
    auto labels = decoded->getRoot<docker_api::Docker::ContainerCreateRequest>().getLabels();
    KJ_EXPECT(labels.getObject().size() == 1);
    auto enabled = readEnableInternetLabel(labels);
    KJ_EXPECT(KJ_ASSERT_NONNULL(enabled) == false);
  }
}

KJ_TEST("ContainerInspectResponse enable-internet label: read; absent or malformed is unknown") {
  // Config.Labels as Docker returns them for a running container: the image's labels and ours.
  auto withLabel = R"({
    "Id": "abc123",
    "State": { "Status": "running", "Running": true },
    "Config": {
      "Image": "cloudflare/workerd/container-client-test",
      "Labels": {
        "org.opencontainers.image.title": "test",
        "dev.workerd.session": "session-1",
        "dev.workerd.enable-internet": "true"
      }
    }
  })"_kj;
  auto message = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(withLabel);
  auto root = message->getRoot<docker_api::Docker::ContainerInspectResponse>();
  KJ_EXPECT(root.getState().getStatus() == "running");
  KJ_EXPECT(root.hasConfig() && root.getConfig().hasLabels());
  auto enabled = readEnableInternetLabel(root.getConfig().getLabels());
  KJ_EXPECT(KJ_ASSERT_NONNULL(enabled) == true);

  // A container created by a workerd from before the label: unknown, not false.
  auto withoutLabel = R"({
    "Id": "abc123",
    "State": { "Status": "running", "Running": true },
    "Config": { "Image": "cloudflare/workerd/container-client-test", "Labels": {} }
  })"_kj;
  auto message2 = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(withoutLabel);
  auto root2 = message2->getRoot<docker_api::Docker::ContainerInspectResponse>();
  KJ_EXPECT(readEnableInternetLabel(root2.getConfig().getLabels()) == kj::none);

  // Labels null (an image without any), or a value that is not "true" / "false": unknown.
  auto nullLabels = R"({
    "Id": "abc123",
    "State": { "Status": "running", "Running": true },
    "Config": { "Image": "x", "Labels": null }
  })"_kj;
  auto message3 = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(nullLabels);
  auto root3 = message3->getRoot<docker_api::Docker::ContainerInspectResponse>();
  KJ_EXPECT(readEnableInternetLabel(root3.getConfig().getLabels()) == kj::none);

  auto malformed = R"({
    "Id": "abc123",
    "State": { "Status": "running", "Running": true },
    "Config": { "Image": "x", "Labels": { "dev.workerd.enable-internet": "maybe" } }
  })"_kj;
  auto message4 = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(malformed);
  auto root4 = message4->getRoot<docker_api::Docker::ContainerInspectResponse>();
  KJ_EXPECT(readEnableInternetLabel(root4.getConfig().getLabels()) == kj::none);
}

}  // namespace
}  // namespace workerd::server
