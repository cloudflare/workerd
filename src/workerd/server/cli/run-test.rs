// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use workerd_capnp::socket;

use super::*;
use crate::wrangler::Kind;
use crate::wrangler::tests::SPAWN;
use crate::wrangler::tests::project;
use crate::wrangler::tests::read;

fn defaults() -> Options {
    Options {
        listen: true,
        port: None,
        compatibility_date: None,
        default_compatibility_date: "2026-10-30".to_owned(),
        compatibility_flags: Vec::new(),
        autogates: Vec::new(),
    }
}

fn sources() -> Vec<Source> {
    vec![(
        "index.js".to_owned(),
        Kind::EsModule,
        b"export default {}".to_vec(),
    )]
}

/// A template, built by `build`.
fn template(
    build: impl FnOnce(config::Builder),
) -> capnp::message::Builder<capnp::message::HeapAllocator> {
    let mut message = capnp::message::Builder::new_default();
    build(message.init_root::<config::Builder>());
    message
}

fn fill_template(
    message: &capnp::message::Builder<capnp::message::HeapAllocator>,
    options: &Options,
) -> Result<Run, String> {
    fill(message.get_root_as_reader().unwrap(), &sources(), options)
}

fn the_worker<'a>(config: config::Reader<'a>, name: &str) -> worker::Reader<'a> {
    let service = config
        .get_services()
        .unwrap()
        .iter()
        .find(|s| s.get_name().unwrap() == name)
        .unwrap();
    let service::Which::Worker(worker) = service.which().unwrap() else {
        panic!("expected a worker service")
    };
    worker.unwrap()
}

fn texts(list: capnp::text_list::Reader) -> Vec<String> {
    list.iter()
        .map(|t| t.unwrap().to_string().unwrap())
        .collect()
}

#[test]
fn runnable_files_are_javascript_or_typescript() {
    for file in ["app.js", "app.mjs", "src/app.ts", "APP.TSX"] {
        assert!(is_runnable(file), "{file}");
    }
    for file in ["app.py", "wrangler.jsonc", "app", "README.md", "-"] {
        assert!(!is_runnable(file), "{file}");
    }
}

#[test]
fn without_a_template() {
    let mut message = capnp::message::Builder::new_default();
    let mut service = message
        .init_root::<config::Builder>()
        .init_services(1)
        .get(0);
    service.set_name("main");
    service.init_worker();
    let options = Options {
        port: Some(9000),
        compatibility_flags: vec!["nodejs_compat".to_owned()],
        autogates: vec!["x".to_owned()],
        ..defaults()
    };
    let Run {
        config,
        worker: name,
        default_port,
    } = fill_template(&message, &options).unwrap();
    // A port was given.
    assert!(!default_port);
    assert_eq!(name, "main");
    let message = read(config);
    let config = message.get_root::<config::Reader>().unwrap();
    let worker = the_worker(config, "main");
    assert_eq!(worker.get_compatibility_date().unwrap(), "2026-10-30");
    assert_eq!(
        texts(worker.get_compatibility_flags().unwrap()),
        ["nodejs_compat"]
    );
    let worker::Which::Modules(modules) = worker.which().unwrap() else {
        panic!("expected a modules worker")
    };
    let modules = modules.unwrap();
    assert_eq!(modules.len(), 1);
    assert_eq!(modules.get(0).get_name().unwrap(), "index.js");
    assert_eq!(
        texts(config.get_autogates().unwrap()),
        ["workerd-autogate-x"]
    );
    let sockets = config.get_sockets().unwrap();
    assert_eq!(sockets.len(), 1);
    let socket = sockets.get(0);
    assert_eq!(socket.get_name().unwrap(), "http");
    assert_eq!(socket.get_address().unwrap(), "localhost:9000");
    assert!(matches!(socket.which().unwrap(), socket::Which::Http(_)));
    assert_eq!(socket.get_service().unwrap().get_name().unwrap(), "main");
}

/// A template with a disk service and a Worker `app` with no modules.
fn app_template() -> capnp::message::Builder<capnp::message::HeapAllocator> {
    template(|conf| {
        let mut services = conf.init_services(2);
        let mut disk = services.reborrow().get(0);
        disk.set_name("files");
        disk.init_disk().set_path("/tmp");
        let mut service = services.get(1);
        service.set_name("app");
        let mut worker = service.init_worker();
        worker.set_compatibility_date("2025-01-01");
        worker.set_compatibility_flags(&["a"][..]).unwrap();
        let mut binding = worker.init_bindings(1).get(0);
        binding.set_name("GREETING");
        binding.set_text("hi");
    })
}

#[test]
fn template_supplies_the_rest() {
    let message = app_template();
    let options = Options {
        compatibility_flags: vec!["a".to_owned(), "b".to_owned()],
        autogates: vec!["workerd-autogate-y".to_owned()],
        ..defaults()
    };
    let Run {
        config,
        worker: name,
        default_port,
    } = fill_template(&message, &options).unwrap();
    // No port was given, and the template has no sockets.
    assert!(default_port);
    assert_eq!(name, "app");
    let message = read(config);
    let config = message.get_root::<config::Reader>().unwrap();
    assert_eq!(config.get_services().unwrap().len(), 2);
    let worker = the_worker(config, "app");
    assert_eq!(worker.get_compatibility_date().unwrap(), "2025-01-01");
    assert_eq!(texts(worker.get_compatibility_flags().unwrap()), ["a", "b"]);
    let bindings = worker.get_bindings().unwrap();
    assert_eq!(bindings.len(), 1);
    assert_eq!(bindings.get(0).get_name().unwrap(), "GREETING");
    assert_eq!(
        texts(config.get_autogates().unwrap()),
        ["workerd-autogate-y"]
    );
    let socket = config.get_sockets().unwrap().get(0);
    assert_eq!(socket.get_address().unwrap(), "localhost:8787");
    assert_eq!(socket.get_service().unwrap().get_name().unwrap(), "app");

    // The command line's date replaces the template's.
    let options = Options {
        compatibility_date: Some("2026-01-01".to_owned()),
        ..defaults()
    };
    let message = read(fill_template(&app_template(), &options).unwrap().config);
    let worker = the_worker(message.get_root().unwrap(), "app");
    assert_eq!(worker.get_compatibility_date().unwrap(), "2026-01-01");
}

#[test]
fn template_sockets_are_kept() {
    let message = template(|mut conf| {
        let mut service = conf.reborrow().init_services(1).get(0);
        service.set_name("main");
        service.init_worker();
        let mut socket = conf.init_sockets(1).get(0);
        socket.set_name("web");
        socket.set_address("*:3000");
        socket.reborrow().init_http();
        socket.init_service().set_name("main");
    });
    let run = fill_template(&message, &defaults()).unwrap();
    assert!(!run.default_port);
    let filled = read(run.config);
    let sockets = filled
        .get_root::<config::Reader>()
        .unwrap()
        .get_sockets()
        .unwrap();
    assert_eq!(sockets.len(), 1);
    assert_eq!(sockets.get(0).get_name().unwrap(), "web");
    assert_eq!(sockets.get(0).get_address().unwrap(), "*:3000");

    let options = Options {
        port: Some(9000),
        ..defaults()
    };
    let error = fill_template(&message, &options).err().unwrap();
    assert!(error.contains("--port=9000"), "{error}");
}

#[test]
fn nothing_listens_under_test() {
    let message = template(|conf| {
        let mut service = conf.init_services(1).get(0);
        service.set_name("main");
        service.init_worker();
    });
    let options = Options {
        listen: false,
        ..defaults()
    };
    let message = read(fill_template(&message, &options).unwrap().config);
    let config = message.get_root::<config::Reader>().unwrap();
    assert!(config.get_sockets().unwrap().is_empty());
}

#[test]
fn template_needs_exactly_one_worker_without_modules() {
    let none = template(|conf| {
        let mut service = conf.init_services(1).get(0);
        service.set_name("main");
        let mut module = service.init_worker().init_modules(1).get(0);
        module.set_name("index.js");
        module.set_es_module("export default {}");
    });
    let error = fill_template(&none, &defaults()).err().unwrap();
    assert!(
        error.contains("Expected a Worker with no modules"),
        "{error}"
    );

    let two = template(|conf| {
        let mut services = conf.init_services(2);
        for (i, name) in ["a", "b"].iter().enumerate() {
            let mut service = services.reborrow().get(i as u32);
            service.set_name(name);
            service.init_worker();
        }
    });
    let error = fill_template(&two, &defaults()).err().unwrap();
    assert!(error.contains("found a, b"), "{error}");
}

/// Installs a stub esbuild as `dir`'s `node_modules/.bin/esbuild`, as `npm i -D esbuild` would.
#[cfg(unix)]
fn install_esbuild(dir: &Path, body: &str) {
    use std::os::unix::fs::PermissionsExt;

    let esbuild = crate::wrangler::tests::stub_esbuild(dir, body);
    let bin = dir.join("node_modules/.bin");
    fs::create_dir_all(&bin).unwrap();
    fs::rename(&esbuild, bin.join("esbuild")).unwrap();
    fs::set_permissions(bin.join("esbuild"), fs::Permissions::from_mode(0o755)).unwrap();
}

#[cfg(unix)]
#[test]
fn a_file_is_bundled_from_its_directory() {
    let _spawn = SPAWN
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let dir = project(&[("app.ts", "export default {}")]);
    install_esbuild(
        &dir,
        r#"echo 'export default {}' > "$out/index.js"
echo '{"inputs":{"app.ts":{}}}' > "$meta""#,
    );
    let process = Process::new(None, None, Vec::new());
    let entry = Entry::File(dir.join("app.ts").to_str().unwrap().to_owned());
    let run = config(&entry, None, &defaults(), &process).unwrap();
    assert!(run.default_port);
    let message = read(run.config);
    let worker = the_worker(message.get_root().unwrap(), "main");
    let worker::Which::Modules(modules) = worker.which().unwrap() else {
        panic!("expected a modules worker")
    };
    assert_eq!(modules.unwrap().get(0).get_name().unwrap(), "index.js");
    let args = fs::read_to_string(dir.join("args.txt")).unwrap();
    assert!(args.lines().any(|l| l == "app.ts"), "{args}");
    assert!(args.lines().any(|l| l == "--entry-names=index"), "{args}");
}

#[cfg(unix)]
#[test]
fn source_is_bundled_from_stdin() {
    let _spawn = SPAWN
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let dir = project(&[("README.md", "")]);
    install_esbuild(
        &dir,
        r#"echo 'export default {}' > "$out/index.js"
echo '{"inputs":{"<stdin>":{}}}' > "$meta""#,
    );
    // Source is bundled from the working directory, where the stub is found.
    let esbuild = dir.join("node_modules/.bin/esbuild");
    let process = Process::new(None, None, Vec::new());
    let source = "export default { fetch: (): Response => new Response('hi') }";
    let sources = wrangler::bundle(
        &esbuild,
        &dir,
        wrangler::Input::Source {
            name: "--eval",
            source,
        },
        &wrangler::watcher(&process),
    )
    .unwrap();
    assert_eq!(sources.len(), 1);
    assert_eq!(sources[0].0, "index.js");
    assert_eq!(fs::read_to_string(dir.join("stdin.txt")).unwrap(), source);
    let args = fs::read_to_string(dir.join("args.txt")).unwrap();
    assert!(args.lines().any(|l| l == "--loader=ts"), "{args}");
    assert!(args.lines().any(|l| l == "--sourcefile=--eval"), "{args}");
    assert!(args.lines().any(|l| l.starts_with("--outfile=")), "{args}");
    // Nothing is written next to the source's imports.
    assert!(!dir.join(".wrangler").exists());
}
