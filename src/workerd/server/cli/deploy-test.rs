use workerd_capnp::config;

use super::*;

/// A fresh directory in the test's temp directory holding `files`.
fn project_dir(files: &[&str]) -> PathBuf {
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static COUNTER: AtomicUsize = AtomicUsize::new(0);
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let dir = tmp.join(format!(
        "workerd-deploy-test-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    for name in files {
        std::fs::write(dir.join(name), "{}").unwrap();
    }
    dir
}

fn path(dir: &Path) -> &str {
    dir.to_str().unwrap()
}

#[test]
fn a_directory_holds_a_wrangler_config() {
    // In wrangler's order.
    let dir = project_dir(&["wrangler.json", "wrangler.jsonc", "wrangler.toml"]);
    assert_eq!(
        project(path(&dir), false),
        Ok(Project::Wrangler(dir.join("wrangler.json")))
    );
    let dir = project_dir(&["wrangler.jsonc", "wrangler.toml"]);
    assert_eq!(
        project(path(&dir), false),
        Ok(Project::Wrangler(dir.join("wrangler.jsonc")))
    );
    let dir = project_dir(&["wrangler.toml"]);
    let error = project(path(&dir), false).unwrap_err();
    assert!(error.contains("wrangler.toml is not supported"), "{error}");
    let dir = project_dir(&[]);
    let error = project(path(&dir), false).unwrap_err();
    assert!(
        error.contains("does not contain wrangler.json or wrangler.jsonc"),
        "{error}"
    );
}

#[test]
fn a_file_is_a_wrangler_config_by_its_extension() {
    let dir = project_dir(&["app.json", "deployment.wd-test"]);
    let wrangler = dir.join("app.json");
    assert_eq!(
        project(path(&wrangler), false),
        Ok(Project::Wrangler(wrangler))
    );
    // A missing path is reported as such, whatever it would have been.
    for missing in ["missing.jsonc", "missing.capnp", "missing"] {
        let error = project(path(&dir.join(missing)), false).unwrap_err();
        assert!(error.contains("No such file or directory"), "{error}");
    }
    std::fs::write(dir.join("wrangler.toml"), "").unwrap();
    let error = project(path(&dir.join("wrangler.toml")), false).unwrap_err();
    assert!(error.contains("wrangler.toml is not supported"), "{error}");

    // Any other file is a Cap'n Proto config, as for `serve`, and so is anything with --binary.
    let capnp = path(&dir.join("deployment.wd-test")).to_owned();
    assert_eq!(project(&capnp, false), Ok(Project::Capnp(capnp.clone())));
    assert_eq!(project("-", true), Ok(Project::Capnp("-".to_owned())));
    assert_eq!(
        project(path(&dir), true),
        Ok(Project::Capnp(path(&dir).to_owned()))
    );
}

/// An encoded `Config` with a worker service named `service` and, with `socket`, a socket.
fn deployment(service: &str, socket: bool) -> Vec<u8> {
    let mut message = capnp::message::Builder::new_default();
    let mut root = message.init_root::<config::Builder>();
    let mut service_builder = root.reborrow().init_services(1).get(0);
    service_builder.set_name(service);
    let mut worker = service_builder.init_worker();
    worker.set_compatibility_date("2026-01-01");
    let mut module = worker.init_modules(1).get(0);
    module.set_name("index.js");
    module.set_es_module("export default {}");
    if socket {
        let mut socket = root.init_sockets(1).get(0);
        socket.set_name("http");
        socket.set_address("*:8080");
        socket.init_service().set_name(service);
    }
    capnp::serialize::write_message_to_words(&message)
}

#[test]
fn a_deployment_is_checked_before_the_bucket_is_opened() {
    assert_eq!(check("app", &deployment("app", false)), Ok(()));
    let refused = check("app", &deployment("app", true)).unwrap_err();
    assert!(
        refused
            .iter()
            .any(|line| line.contains("A deployment may contain only `services`")),
        "{refused:?}"
    );
    let refused = check("my.app", &deployment("app", false)).unwrap_err();
    assert!(
        refused[0].contains("is not a valid deployment name"),
        "{refused:?}"
    );
}

#[test]
fn the_help_names_the_deployment_storage() {
    use clap::CommandFactory;
    let help = crate::args::Main::command()
        .find_subcommand_mut("deploy")
        .unwrap()
        .render_long_help()
        .to_string();
    assert!(
        help.contains(&format!("`{}`", wrangler::DEPLOYMENT_STORAGE)),
        "{help}"
    );
}
