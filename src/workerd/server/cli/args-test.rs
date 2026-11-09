use clap::CommandFactory;

use super::*;

fn parse(args: &[&str]) -> Main {
    Main::try_parse_from(std::iter::once("workerd").chain(args.iter().copied())).unwrap()
}

#[test]
fn definitions_are_consistent() {
    Main::command().debug_assert();
    CompiledMain::command().debug_assert();
}

#[test]
fn wd_test_invocation() {
    let Command::Test {
        config,
        serve_or_test,
        test,
    } = parse(&[
        "test",
        "--predictable",
        "foo.wd-test",
        "--compat-date=2000-01-01",
        "-dTEST_TMPDIR=/tmp/x",
        "--socket-addr=tcp=127.0.0.1:1234",
    ])
    .command
    else {
        panic!("expected test")
    };
    assert_eq!(config.config_file, "foo.wd-test");
    assert!(test.predictable);
    assert_eq!(test.compat_date.as_deref(), Some("2000-01-01"));
    assert_eq!(test.filter, None);
    assert_eq!(
        test.socket_addr_overrides,
        [Override {
            name: "tcp".into(),
            value: "127.0.0.1:1234".into()
        }]
    );
    assert_eq!(
        serve_or_test.directory_overrides,
        [Override {
            name: "TEST_TMPDIR".into(),
            value: "/tmp/x".into()
        }]
    );
}

#[test]
fn serve_options() {
    let Command::Serve {
        config,
        const_name,
        serve_or_test,
        serve,
    } = parse(&[
        "serve",
        "--binary",
        "-",
        "-I",
        "inc",
        "-wb",
        "--experimental",
        "--socket-addr",
        "http=*:8080",
        "--control-fd=3",
        "--inspector-addr=127.0.0.1:9229",
        "--inspector-addr=127.0.0.1:9230",
        "--async-trace=trace.ndjson",
        "--async-trace-stacks=8",
        "--async-trace-promises",
        "--cluster-addr=0.0.0.0:7000",
        "--cluster-advertise=10.0.0.4:7000",
    ])
    .command
    else {
        panic!("expected serve")
    };
    assert_eq!(config.config_file, "-");
    assert!(config.binary);
    assert_eq!(config.import_paths, ["inc"]);
    assert_eq!(const_name.const_name, None);
    assert!(serve_or_test.watch);
    assert!(serve_or_test.experimental);
    // Later occurrences override earlier ones, as with kj::MainBuilder.
    assert_eq!(
        serve_or_test.inspector_addr.as_deref(),
        Some("127.0.0.1:9230")
    );
    assert_eq!(serve_or_test.async_trace.as_deref(), Some("trace.ndjson"));
    assert_eq!(serve_or_test.async_trace_stacks, Some(8));
    assert!(serve_or_test.async_trace_promises);
    assert_eq!(serve.control_fd, Some(3));
    assert_eq!(serve.cluster_address.as_deref(), Some("0.0.0.0:7000"));
    assert_eq!(serve.cluster_advertise.as_deref(), Some("10.0.0.4:7000"));
    assert_eq!(
        serve.socket_addr_overrides,
        [Override {
            name: "http".into(),
            value: "*:8080".into()
        }]
    );
}

#[test]
fn serve_const_name_and_verbose_after_subcommand() {
    let main = parse(&["serve", "config.capnp", "myConfig", "--verbose"]);
    assert!(main.global.verbose);
    let Command::Serve { const_name, .. } = main.command else {
        panic!("expected serve")
    };
    assert_eq!(const_name.const_name.as_deref(), Some("myConfig"));
}

#[test]
fn test_filters() {
    let filter = |param| parse_test_filter(param);
    assert_eq!(
        filter("svc"),
        Ok(TestFilter {
            service_pattern: Some("svc".into()),
            ..TestFilter::default()
        })
    );
    assert_eq!(
        filter("svc:ep"),
        Ok(TestFilter {
            service_pattern: Some("svc".into()),
            entrypoint_pattern: Some("ep".into()),
            ..TestFilter::default()
        })
    );
    assert_eq!(
        filter("cfg:svc:"),
        Ok(TestFilter {
            const_name: Some("cfg".into()),
            service_pattern: Some("svc".into()),
            entrypoint_pattern: Some(String::new()),
        })
    );
    assert_eq!(filter("a:b:c:d"), Err("Too many colons."));
}

#[test]
fn bench_filters() {
    let filter = |param| parse_bench_filter(param);
    assert_eq!(
        filter("svc:ep/encode*"),
        Ok(BenchFilter {
            test: TestFilter {
                service_pattern: Some("svc".into()),
                entrypoint_pattern: Some("ep".into()),
                ..TestFilter::default()
            },
            case_pattern: Some("encode*".into()),
        })
    );
    assert_eq!(
        filter("/a/b"),
        Ok(BenchFilter {
            case_pattern: Some("a/b".into()),
            ..BenchFilter::default()
        })
    );
    assert_eq!(
        filter("svc"),
        Ok(BenchFilter {
            test: TestFilter {
                service_pattern: Some("svc".into()),
                ..TestFilter::default()
            },
            case_pattern: None,
        })
    );
    assert_eq!(filter("a:b:c:d/x"), Err("Too many colons."));
}

#[test]
fn bench_invocation() {
    let Command::Bench { config, bench, .. } = parse(&[
        "bench",
        "foo.wd-test",
        "--quick",
        "--format=json",
        "--output=out.json",
        "--trace",
        "svc/case",
    ])
    .command
    else {
        panic!("expected bench")
    };
    assert_eq!(config.config_file, "foo.wd-test");
    assert!(bench.quick);
    assert_eq!(bench.format, BenchFormat::Json);
    assert_eq!(bench.output.as_deref(), Some("out.json"));
    assert!(bench.trace);
    assert_eq!(
        bench.filter.map(|f| f.case_pattern),
        Some(Some("case".into()))
    );
}

#[test]
fn bad_values_are_usage_errors() {
    for args in [
        &["serve", "c.capnp", "--socket-addr=noequals"][..],
        &["serve", "c.capnp", "--socket-fd=http=abc"],
        &["serve", "c.capnp", "--control-fd=-1"],
        &["serve"],
        &["test", "c.capnp", "a", "b"],
        &["unknown"],
        // A deploy needs the cluster's bucket, as s3://<bucket>[/<prefix>].
        &["deploy"],
        &["deploy", "--bucket=gs://cluster"],
        &["deploy", "--bucket=s3://"],
        &["deploy", "--bucket=s3:///prefix"],
        &["deploy", "--bucket=cluster"],
        &[
            "deploy",
            "--bucket=s3://cluster",
            "--endpoint=s3.example.com",
        ],
        &["deploy", "--bucket=s3://cluster", "--endpoint=https://"],
        &["deploy", "--bucket=s3://cluster", "--endpoint=http:///"],
        &[
            "deploy",
            "--bucket=s3://cluster",
            "--endpoint=http://host/base",
        ],
        &[
            "deploy",
            "--bucket=s3://cluster",
            "--endpoint=http://host?x=1",
        ],
        &[
            "deploy",
            "--bucket=s3://cluster",
            "--endpoint=http://user@host",
        ],
        &[
            "deploy",
            "--bucket=s3://cluster",
            "app.capnp",
            "appConfig",
            "extra",
        ],
    ] {
        let argv = std::iter::once("workerd").chain(args.iter().copied());
        assert!(Main::try_parse_from(argv).is_err(), "{args:?}");
    }
}

#[test]
fn socket_overridden_twice_finds_the_conflict() {
    let Command::Serve { serve, .. } =
        parse(&["serve", "c.capnp", "-shttp=*:8080", "-Sadmin=3", "-Shttp=4"]).command
    else {
        panic!("expected serve")
    };
    assert_eq!(socket_overridden_twice(&serve), Some("http"));
    let Command::Serve { serve, .. } =
        parse(&["serve", "c.capnp", "-shttp=*:8080", "-Sadmin=3"]).command
    else {
        panic!("expected serve")
    };
    assert_eq!(socket_overridden_twice(&serve), None);
}

#[test]
fn deploy_invocation() {
    let Command::Deploy {
        project,
        const_name,
        deploy,
    } = parse(&[
        "deploy",
        "--experimental",
        "-I",
        "inc",
        "app.capnp",
        "appConfig",
        "--name=app",
        "--bucket",
        "s3://cluster/prefix",
        "--endpoint=http://127.0.0.1:9000",
        "--region=auto",
    ])
    .command
    else {
        panic!("expected deploy")
    };
    assert_eq!(project, "app.capnp");
    assert_eq!(const_name.as_deref(), Some("appConfig"));
    assert_eq!(deploy.name.as_deref(), Some("app"));
    assert_eq!(deploy.import_paths, ["inc"]);
    assert_eq!(deploy.bucket, "s3://cluster/prefix");
    assert_eq!(
        deploy.bucket_options(),
        ["aws_endpoint=http://127.0.0.1:9000", "aws_region=auto"]
    );
    assert!(deploy.experimental);
    assert!(!deploy.binary);

    // The project defaults to the working directory, and the endpoint, region and credentials
    // to the environment.
    let Command::Deploy {
        project,
        const_name,
        deploy,
    } = parse(&["deploy", "--bucket=s3://cluster"]).command
    else {
        panic!("expected deploy")
    };
    assert_eq!(project, ".");
    assert_eq!(const_name, None);
    assert_eq!(deploy.name, None);
    assert!(deploy.bucket_options().is_empty());
}

#[test]
fn deploy_bucket_and_endpoint_forms() {
    let deploy = |args: &[&str]| {
        let Command::Deploy { deploy, .. } = parse(args).command else {
            panic!("expected deploy")
        };
        deploy
    };
    for bucket in ["s3://cluster", "s3://cluster/", "s3://cluster/a/b"] {
        let flag = format!("--bucket={bucket}");
        assert_eq!(deploy(&["deploy", &flag]).bucket, bucket);
    }
    for endpoint in [
        "http://127.0.0.1:9000",
        "https://s3.example.com/",
        "https://[::1]:9000",
    ] {
        let flag = format!("--endpoint={endpoint}");
        let parsed = deploy(&["deploy", "--bucket=s3://cluster", &flag]);
        assert_eq!(parsed.endpoint.as_deref(), Some(endpoint));
    }
}

#[test]
fn compiled_binary_has_no_config_arguments() {
    let main = CompiledMain::try_parse_from(["workerd", "-s", "http=*:8080"]).unwrap();
    assert_eq!(main.serve.socket_addr_overrides.len(), 1);
    assert!(CompiledMain::try_parse_from(["workerd", "config.capnp"]).is_err());
}
