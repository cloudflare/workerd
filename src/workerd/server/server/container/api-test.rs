// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn container_inspect_response() {
    // What Docker returns for `/containers/<id>/json`, trimmed.
    let json = r#"{
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
        "Gateway": "172.17.0.1",
        "IPAddress": "172.17.0.2",
        "IPPrefixLen": 16,
        "MacAddress": "02:42:ac:11:00:02",
        "Networks": {},
        "Ports": {
          "8080/tcp": [
            {"HostIp": "0.0.0.0", "HostPort": "55000"}
          ]
        }
      }
    }"#;
    let root: ContainerInspectResponse = serde_json::from_str(json).unwrap();
    assert_eq!(root.id.as_deref(), Some("abc123"));
    let state = root.state.unwrap();
    assert_eq!(state.status.as_deref(), Some("running"));
    assert_eq!(state.running, Some(true));
    assert_eq!(
        root.args.unwrap(),
        ["--http-egress-port".to_owned(), "9000".to_owned()]
    );
    let ports = root.network_settings.unwrap().ports.unwrap();
    assert_eq!(ports.len(), 1);
    let bindings = ports["8080/tcp"].as_array().unwrap();
    assert_eq!(bindings.len(), 1);
    assert_eq!(bindings[0]["HostPort"], "55000");
    assert!(root.config.is_none());
}

#[test]
fn container_inspect_response_with_null_ports() {
    let json = r#"{"State": {"Status": "exited"}, "NetworkSettings": {"Ports": null}}"#;
    let root: ContainerInspectResponse = serde_json::from_str(json).unwrap();
    assert!(root.network_settings.unwrap().ports.is_none());
}

#[test]
fn network_inspect_response() {
    let json = r#"{
      "Name": "bridge",
      "Id": "net123",
      "IPAM": {
        "Driver": "default",
        "Config": [
          {"Subnet": "172.17.0.0/16", "Gateway": "172.17.0.1"}
        ]
      }
    }"#;
    let root: NetworkInspectResponse = serde_json::from_str(json).unwrap();
    assert_eq!(root.name.as_deref(), Some("bridge"));
    let config = root.ipam.unwrap().config.unwrap();
    assert_eq!(config.len(), 1);
    assert_eq!(config[0].subnet, "172.17.0.0/16");
    assert_eq!(config[0].gateway, "172.17.0.1");
}

#[test]
fn container_wait_response() {
    let root: ContainerWaitResponse = serde_json::from_str(r#"{"StatusCode": 0}"#).unwrap();
    assert_eq!(root.status_code, 0);
}

#[test]
fn container_wait_response_non_zero_exit() {
    let root: ContainerWaitResponse = serde_json::from_str(r#"{"StatusCode": 137}"#).unwrap();
    assert_eq!(root.status_code, 137);
}

#[test]
fn exec_inspect_response() {
    let running: ExecInspectResponse =
        serde_json::from_str(r#"{"Running": true, "Pid": 7, "ExitCode": null}"#).unwrap();
    assert!(running.running);
    assert_eq!(running.pid, 7);
    assert!(running.exit_code.is_none());
    let exited: ExecInspectResponse =
        serde_json::from_str(r#"{"Running": false, "Pid": 0, "ExitCode": 3}"#).unwrap();
    assert_eq!(exited.exit_code.unwrap().as_f64(), Some(3.0));
}

fn create_request(host_config: HostConfig) -> ContainerCreateRequest {
    ContainerCreateRequest {
        image: Some("test-image".to_owned()),
        host_config: Some(host_config),
        ..ContainerCreateRequest::default()
    }
}

#[test]
fn create_request_encodes_structured_mounts_with_no_copy() {
    let request = create_request(HostConfig {
        mounts: Some(vec![Mount {
            kind: "volume".to_owned(),
            source: "snapshot-clone-volume".to_owned(),
            target: "/app/data".to_owned(),
            read_only: false,
            volume_options: Some(VolumeOptions { no_copy: true }),
        }]),
        ..HostConfig::default()
    });
    let json = serde_json::to_string(&request).unwrap();
    assert!(json.contains("\"Mounts\""));
    assert!(json.contains("\"VolumeOptions\""));
    assert!(json.contains("\"NoCopy\":true"));

    let decoded: ContainerCreateRequest = serde_json::from_str(&json).unwrap();
    let mounts = decoded.host_config.unwrap().mounts.unwrap();
    assert_eq!(mounts.len(), 1);
    assert_eq!(mounts[0].kind, "volume");
    assert_eq!(mounts[0].source, "snapshot-clone-volume");
    assert_eq!(mounts[0].target, "/app/data");
    assert!(mounts[0].volume_options.as_ref().unwrap().no_copy);
}

#[test]
fn create_request_encodes_entrypoint_as_array() {
    let request = ContainerCreateRequest {
        image: Some("test-image".to_owned()),
        entrypoint: Some(vec!["/bin/cp".to_owned()]),
        ..ContainerCreateRequest::default()
    };
    let json = serde_json::to_string(&request).unwrap();
    assert!(json.contains("\"Entrypoint\":[\"/bin/cp\"]"));
    let decoded: ContainerCreateRequest = serde_json::from_str(&json).unwrap();
    assert_eq!(decoded.entrypoint.unwrap(), ["/bin/cp".to_owned()]);
}

#[test]
fn create_request_encodes_host_config_dns() {
    let request = create_request(HostConfig {
        dns: Some(vec!["1.1.1.1".to_owned(), "8.8.8.8".to_owned()]),
        ..HostConfig::default()
    });
    let json = serde_json::to_string(&request).unwrap();
    assert!(json.contains("\"Dns\""));
    assert!(json.contains("1.1.1.1"));
    assert!(json.contains("8.8.8.8"));
    let decoded: ContainerCreateRequest = serde_json::from_str(&json).unwrap();
    assert_eq!(
        decoded.host_config.unwrap().dns.unwrap(),
        ["1.1.1.1".to_owned(), "8.8.8.8".to_owned()]
    );
}

#[test]
fn create_request_encodes_configured_privileges() {
    let mut host_config = HostConfig::default();
    host_config.grant(&Privileges {
        capabilities: vec!["SYS_ADMIN".to_owned()],
        devices: vec![Device {
            path_on_host: "/dev/fuse".to_owned(),
            path_in_container: "/dev/fuse".to_owned(),
            cgroup_permissions: "rwm".to_owned(),
        }],
        security_opt: vec!["apparmor:unconfined".to_owned()],
    });
    let json = serde_json::to_string(&create_request(host_config)).unwrap();
    assert!(json.contains("\"CapAdd\""));
    assert!(json.contains("\"SYS_ADMIN\""));
    assert!(json.contains("\"Devices\""));
    assert!(json.contains("\"PathOnHost\":\"/dev/fuse\""));
    assert!(json.contains("\"PathInContainer\":\"/dev/fuse\""));
    assert!(json.contains("\"CgroupPermissions\":\"rwm\""));
    assert!(json.contains("\"SecurityOpt\""));
    assert!(json.contains("\"apparmor:unconfined\""));

    let decoded: ContainerCreateRequest = serde_json::from_str(&json).unwrap();
    let host_config = decoded.host_config.unwrap();
    assert_eq!(host_config.cap_add.unwrap(), ["SYS_ADMIN".to_owned()]);
    let devices = host_config.devices.unwrap();
    assert_eq!(devices.len(), 1);
    assert_eq!(devices[0].path_on_host, "/dev/fuse");
    assert_eq!(devices[0].path_in_container, "/dev/fuse");
    assert_eq!(devices[0].cgroup_permissions, "rwm");
    assert_eq!(
        host_config.security_opt.unwrap(),
        ["apparmor:unconfined".to_owned()]
    );
}

#[test]
fn create_request_omits_empty_privileges() {
    let mut host_config = HostConfig::default();
    host_config.grant(&Privileges::default());
    let json = serde_json::to_string(&create_request(host_config)).unwrap();
    for absent in [
        "CapAdd",
        "SYS_ADMIN",
        "Devices",
        "PathOnHost",
        "PathInContainer",
        "CgroupPermissions",
        "SecurityOpt",
    ] {
        assert!(!json.contains(absent), "{json} contains {absent}");
    }
    let decoded: ContainerCreateRequest = serde_json::from_str(&json).unwrap();
    let host_config = decoded.host_config.unwrap();
    assert!(host_config.cap_add.is_none());
    assert!(host_config.devices.is_none());
    assert!(host_config.security_opt.is_none());
}

#[test]
fn exec_requests_leave_defaults_out() {
    let start = ExecStartRequest::default();
    assert_eq!(serde_json::to_string(&start).unwrap(), "{}");
    let start = ExecStartRequest {
        tty: Some(true),
        console_size: Some([24, 80]),
        ..ExecStartRequest::default()
    };
    assert_eq!(
        serde_json::to_string(&start).unwrap(),
        r#"{"Tty":true,"ConsoleSize":[24,80]}"#
    );
    let create = ExecCreateRequest {
        attach_stdin: Some(true),
        cmd: vec!["ls".to_owned()],
        ..ExecCreateRequest::default()
    };
    assert_eq!(
        serde_json::to_string(&create).unwrap(),
        r#"{"AttachStdin":true,"Cmd":["ls"]}"#
    );
}

#[test]
fn egress_config() {
    let config = EgressConfig {
        port: 1234,
        dns: Some(EgressDns {
            allow_hostnames: vec![],
        }),
        internet: Some(EgressInternet { enabled: None }),
    };
    assert_eq!(
        serde_json::to_string(&config).unwrap(),
        r#"{"port":1234,"dns":{"allowHostnames":[]},"internet":{}}"#
    );
    let port_only = EgressConfig {
        port: 1,
        ..EgressConfig::default()
    };
    assert_eq!(serde_json::to_string(&port_only).unwrap(), r#"{"port":1}"#);
}

#[test]
fn volume_list_filters() {
    let filters = VolumeListFilters {
        name: vec!["workerd-snap-".to_owned()],
    };
    assert_eq!(
        serde_json::to_string(&filters).unwrap(),
        r#"{"name":["workerd-snap-"]}"#
    );
}

#[test]
fn create_request_sends_every_scalar() {
    let json = serde_json::to_value(create_request(HostConfig::default())).unwrap();
    assert_eq!(json["StopTimeout"], 10);
    assert_eq!(json["AttachStdin"], false);
    assert_eq!(json["NetworkDisabled"], false);
    let host = &json["HostConfig"];
    assert_eq!(host["MemorySwappiness"], 0);
    assert_eq!(host["PidsLimit"], 0);
    assert_eq!(host["MaximumIOps"], 0);
    assert_eq!(host["MaximumIOBps"], 0);
    assert_eq!(host["Privileged"], false);
    assert_eq!(host["PublishAllPorts"], false);
    assert!(host.get("Binds").is_none());
}
