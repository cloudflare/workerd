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
        "--data-dir=/var/lib/workerd",
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
    assert_eq!(serve_or_test.data_dir.as_deref(), Some("/var/lib/workerd"));
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
        "--no-startup",
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
    assert!(bench.no_startup);
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
        &["serve", "c.capnp", "--threads=0"],
        &["serve", "c.capnp", "--threads=257"],
        &["serve", "c.capnp", "--threads=two"],
        &["serve", "c.capnp", "--threads"],
        &["serve", "c.capnp", "--front-threads=0"],
        &["serve", "c.capnp", "--front-threads=257"],
        // Only `serve` has threads.
        &["test", "c.capnp", "--threads=2"],
        &["bench", "c.capnp", "--threads=2"],
        &["serve"],
        &["test", "c.capnp", "a", "b"],
        &["unknown"],
        // A deploy needs the cluster's bucket, as s3://<bucket>[/<prefix>] or
        // gs://<bucket>[/<prefix>].
        &["deploy"],
        &["deploy", "--bucket=az://cluster"],
        &["deploy", "--bucket=s3://"],
        &["deploy", "--bucket=s3:///prefix"],
        &["deploy", "--bucket=gs://"],
        &["deploy", "--bucket=gs:///prefix"],
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
        deploy.bucket_options().unwrap(),
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
    assert!(deploy.bucket_options().unwrap().is_empty());
}

#[test]
fn a_gs_bucket_refuses_s3_flags() {
    let options = |args: &[&str]| {
        let Command::Deploy { deploy, .. } = parse(args).command else {
            panic!("expected deploy")
        };
        deploy.bucket_options()
    };
    assert_eq!(
        options(&["deploy", "--bucket=gs://cluster"]),
        Ok(Vec::new())
    );
    for (flag, name) in [
        ("--endpoint=http://127.0.0.1:9000", "--endpoint"),
        ("--region=auto", "--region"),
    ] {
        assert_eq!(
            options(&["deploy", "--bucket=gs://cluster/prefix", flag]),
            Err(format!(
                "{name} is for an s3:// bucket. Remove it for the gs:// bucket."
            ))
        );
    }
}

#[test]
fn deploy_bucket_and_endpoint_forms() {
    let deploy = |args: &[&str]| {
        let Command::Deploy { deploy, .. } = parse(args).command else {
            panic!("expected deploy")
        };
        deploy
    };
    for bucket in [
        "s3://cluster",
        "s3://cluster/",
        "s3://cluster/a/b",
        "gs://cluster",
        "gs://cluster/a/b",
    ] {
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

/// Every help page fits 80 columns and lists each of its command's options, and `workerd -h`
/// names every command, so the hand-written pages in help.rs cannot drift from the parser.
#[test]
fn help_pages_are_complete() {
    // `flag` as a whole word, so that `-d` is not found in `--directory-path`.
    fn has(page: &str, flag: &str) -> bool {
        page.split([' ', ',', '\n']).any(|word| word == flag)
    }
    fn check(cmd: &mut clap::Command, path: &str) {
        let page = cmd.render_help().to_string();
        for line in page.lines() {
            assert!(
                line.chars().count() <= 80,
                "{path}: over 80 columns: {line}"
            );
            assert!(
                !(line.len() > 37 && line[..37].trim().is_empty()),
                "{path}: a row wraps: {line}"
            );
            assert!(!line.starts_with('-'), "{path}: unindented: {line}");
        }
        for arg in cmd.get_arguments() {
            let long = arg.get_long().map(|l| format!("--{l}"));
            let short = arg.get_short().map(|s| format!("-{s}"));
            for flag in long.iter().chain(short.iter()) {
                let shared =
                    page.contains(crate::help::SHARED_NOTE) && has(crate::help::SHARED, flag);
                assert!(shared || has(&page, flag), "{path}: no {flag}");
            }
        }
    }
    let mut main = Main::command();
    main.build();
    let page = main.render_help().to_string();
    check(&mut main, "workerd");
    for sub in main.get_subcommands_mut() {
        let name = sub.get_name().to_owned();
        if !sub.is_hide_set() && name != "help" {
            assert!(
                page.contains(&format!("workerd {name} ")),
                "workerd -h: no {name}"
            );
        }
        if name != "help" {
            check(sub, &format!("workerd {name}"));
        }
    }
    let mut compiled = CompiledMain::command();
    compiled.build();
    check(&mut compiled, "compiled");
}

#[test]
fn repl_config_to_load() {
    let repl = |args: &[&str]| {
        let Command::Repl { repl } = parse(args).command else {
            panic!("not repl: {args:?}");
        };
        repl
    };
    let none = repl(&["repl", "--experimental"]);
    assert_eq!(none.config_file, None);
    let config = repl(&["repl", "--experimental", "config.capnp", "dev"]);
    assert_eq!(config.config_file.as_deref(), Some("config.capnp"));
    assert_eq!(config.const_name.as_deref(), Some("dev"));
}

#[test]
fn repl_input_timeout() {
    let timeout = |args: &[&str]| {
        let Command::Repl { repl } = parse(args).command else {
            panic!("not repl: {args:?}");
        };
        repl.input_timeout
    };
    assert_eq!(timeout(&["repl", "--experimental"]), None);
    assert_eq!(
        timeout(&["repl", "--experimental", "--input-timeout=30"]),
        Some(Duration::from_secs(30))
    );
    assert_eq!(
        timeout(&["repl", "--experimental", "--input-timeout", "0.5"]),
        Some(Duration::from_millis(500))
    );
    for seconds in ["0", "-1", "NaN", "inf", "soon"] {
        let arg = format!("--input-timeout={seconds}");
        assert!(
            Main::try_parse_from(["workerd", "repl", "--experimental", &arg]).is_err(),
            "{arg}"
        );
    }
}

#[test]
fn repl_process_wide_settings() {
    let main = parse(&[
        "repl",
        "--experimental",
        "--autogate=compression-rs",
        "--autogate",
        "ts-url",
        "--v8-flag=--expose-gc",
        // A V8 flag looks like an option of its own.
        "--v8-flag",
        "--jitless",
        "config.capnp",
    ]);
    let Command::Repl { repl } = main.command else {
        panic!("not repl");
    };
    assert_eq!(repl.autogates, ["compression-rs", "ts-url"]);
    assert_eq!(repl.v8_flags, ["--expose-gc", "--jitless"]);
    assert_eq!(repl.config_file.as_deref(), Some("config.capnp"));
}
/// The `serve` arguments of `workerd serve c.capnp <args>`.
fn serve_args(args: &[&str]) -> (ServeArgs, ServeOrTestArgs) {
    let Command::Serve {
        serve,
        serve_or_test,
        ..
    } = parse(&[&["serve", "c.capnp"], args].concat()).command
    else {
        panic!("expected serve")
    };
    (serve, serve_or_test)
}

#[test]
fn threads_default_to_one() {
    let (serve, serve_or_test) = serve_args(&[]);
    assert_eq!(serve.threads, 1);
    assert_eq!(threads_refused(&serve, &serve_or_test), None);
    let (serve, _) = serve_args(&["--threads=4"]);
    assert_eq!(serve.threads, 4);
    let main = CompiledMain::try_parse_from(["workerd", "--threads=3"]).unwrap();
    assert_eq!(main.serve.threads, 3);
}

#[test]
fn the_front_has_threads_only_with_several_loop_threads() {
    let (serve, _) = serve_args(&["--threads=4", "--front-threads=3"]);
    assert_eq!(serve.front_threads, Some(3));
    let (serve, _) = serve_args(&["--threads=4"]);
    assert_eq!(serve.front_threads, None);
    // One thread has no front.
    for args in [
        &["--front-threads=2"][..],
        &["--threads=1", "--front-threads=2"],
    ] {
        let (serve, serve_or_test) = serve_args(args);
        let message = threads_refused(&serve, &serve_or_test).unwrap();
        assert!(message.starts_with("--front-threads: "), "{message}");
    }
    // It is part of serving on several threads, which is experimental.
    let (serve, serve_or_test) = serve_args(&["--threads=2", "--front-threads=2"]);
    let message = threads_refused(&serve, &serve_or_test).unwrap();
    assert!(message.contains("`--experimental`"), "{message}");
}

#[test]
fn one_thread_is_refused_nothing() {
    // One thread is the server as it is without the flag, whatever else is given.
    for args in [
        &["--threads=1"][..],
        &["--threads=1", "--inspector-addr=127.0.0.1:0"],
        &["--threads=1", "--experimental", "--python-save-snapshot"],
        &["--inspector-addr=127.0.0.1:0", "--python-save-snapshot"],
    ] {
        let (serve, serve_or_test) = serve_args(args);
        let refused = threads_refused(&serve, &serve_or_test);
        assert_eq!(refused, None, "{args:?}");
    }
}

#[test]
fn several_threads_refuse_what_they_do_not_support() {
    let refused = |args: &[&str]| {
        let (serve, serve_or_test) = serve_args(args);
        threads_refused(&serve, &serve_or_test)
    };
    let experimental = refused(&["--threads=2"]).unwrap();
    assert!(experimental.contains("`--experimental`"), "{experimental}");
    // Without --experimental that is the error, whatever else is given.
    assert_eq!(
        refused(&["--threads=2", "--inspector-addr=127.0.0.1:0"]),
        Some(experimental)
    );

    for (option, named) in [
        ("--inspector-addr=127.0.0.1:0", "--inspector-addr"),
        ("-i127.0.0.1:0", "--inspector-addr"),
        ("--python-save-snapshot", "Python snapshot"),
        ("--python-save-baseline-snapshot", "Python snapshot"),
    ] {
        let message = refused(&["--threads=2", "--experimental", option]).unwrap();
        assert!(message.starts_with("--threads: "), "{message}");
        assert!(message.contains(named), "{option}: {message}");
    }

    // What several threads serve, or one of them does.
    for args in [
        &["--threads=2", "--experimental"][..],
        &["--threads=256", "--experimental"],
        &["--threads=2", "--experimental", "--watch"],
        &["--threads=2", "--experimental", "--control-fd=3"],
        &["--threads=2", "--experimental", "-Shttp=4"],
        &["--threads=2", "--experimental", "-shttp=*:8080"],
        &["--threads=2", "--experimental", "--debug-port=127.0.0.1:0"],
        &["--threads=2", "--experimental", "--python-load-snapshot=x"],
        &["--threads=2", "--experimental", "--async-trace=trace"],
        &["--threads=2", "--experimental", "--front-threads=1"],
        &["--threads=2", "--experimental", "--front-threads=256"],
    ] {
        assert_eq!(refused(args), None, "{args:?}");
    }
}
