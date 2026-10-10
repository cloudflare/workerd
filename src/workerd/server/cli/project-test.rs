use std::fs;

use super::*;

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!(
        "workerd-project-test-{name}-{}",
        std::process::id()
    ));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn jsonc_comments_and_trailing_commas_are_stripped() {
    let text = r#"{
      // a line comment
      "a": 1, /* block
      comment */
      "b": ["x", "y",],
      "c": "has // no comment inside",
    }"#;
    let value: Value = serde_json::from_str(&strip_jsonc(text)).unwrap();
    assert_eq!(value["a"], 1);
    assert_eq!(value["b"], serde_json::json!(["x", "y"]));
    assert_eq!(value["c"], "has // no comment inside");
}

#[test]
fn const_names_are_identifiers() {
    assert_eq!(const_name("my-worker"), "myWorkerWorker");
    assert_eq!(const_name("app"), "appWorker");
    assert_eq!(const_name("1st"), "_1stWorker");
    assert_eq!(const_name("---"), "workerWorker");
}

#[test]
fn quoting_escapes_capnp_text() {
    assert_eq!(quote("a\"b\\c\n"), "\"a\\\"b\\\\c\\n\"");
}

#[test]
fn source_file_project_collects_siblings_main_first() {
    let dir = temp_dir("source");
    fs::write(dir.join("app.js"), "import './lib/util.js';").unwrap();
    fs::create_dir_all(dir.join("lib")).unwrap();
    fs::write(dir.join("lib/util.js"), "export const x = 1;").unwrap();
    fs::write(dir.join("data.json"), "{}").unwrap();
    fs::create_dir_all(dir.join("node_modules/pkg")).unwrap();
    fs::write(dir.join("node_modules/pkg/index.js"), "").unwrap();
    fs::write(dir.join("README"), "not a module").unwrap();

    let project = Project::from_source_file(
        dir.join("app.js").to_str().unwrap(),
        "2026-01-01",
        &Overrides::default(),
    )
    .unwrap();
    let names: Vec<&str> = project.modules.iter().map(|m| m.name.as_str()).collect();
    assert_eq!(names, ["app.js", "data.json", "lib/util.js"]);
    assert_eq!(project.name, "app");
    assert_eq!(project.compatibility_date, "2026-01-01");
    assert_eq!(project.compatibility_flags, ["nodejs_compat"]);
    assert_eq!(project.port, DEFAULT_RUN_PORT);
    assert_eq!(project.persist, None);

    let config = project.to_config().unwrap();
    assert!(!config.into_words().is_empty());
    let text = project.to_capnp_text();
    assert!(text.contains("(name = \"app.js\", esModule = embed \"app.js\")"));
    assert!(text.contains("address = \"*:8080\""));
}

#[test]
fn typescript_entry_adds_strip_types_flag() {
    let dir = temp_dir("ts");
    fs::write(dir.join("app.ts"), "export default {};").unwrap();
    let project = Project::from_source_file(
        dir.join("app.ts").to_str().unwrap(),
        "2026-01-01",
        &Overrides::default(),
    )
    .unwrap();
    assert!(project.needs_experimental());
    assert!(
        project
            .compatibility_flags
            .contains(&"typescript_strip_types".to_owned())
    );
}

#[test]
fn wrangler_config_maps_bindings_and_warns_about_the_rest() {
    let dir = temp_dir("wrangler");
    fs::create_dir_all(dir.join("src")).unwrap();
    fs::write(dir.join("src/index.js"), "export default {};").unwrap();
    fs::write(
        dir.join("wrangler.jsonc"),
        r#"{
          // the worker
          "name": "demo",
          "main": "src/index.js",
          "compatibility_date": "2025-05-05",
          "compatibility_flags": ["nodejs_compat"],
          "vars": { "GREETING": "hi", "LIMITS": { "max": 3 } },
          "durable_objects": { "bindings": [ { "name": "COUNTER", "class_name": "Counter" } ] },
          "migrations": [ { "tag": "v1", "new_sqlite_classes": ["Counter"] } ],
          "services": [ { "binding": "SELF", "service": "demo" } ],
          "kv_namespaces": [ { "binding": "KV", "id": "abc" } ],
          "dev": { "port": 9999 },
        }"#,
    )
    .unwrap();

    let project = Project::from_wrangler_config(
        dir.join("wrangler.jsonc").to_str().unwrap(),
        "2026-01-01",
        &Overrides {
            port: Some(1234),
            ..Overrides::default()
        },
    )
    .unwrap();
    assert_eq!(project.name, "demo");
    assert_eq!(project.port, 1234);
    assert_eq!(project.compatibility_date, "2025-05-05");
    assert_eq!(project.modules[0].name, "src/index.js");
    assert_eq!(project.vars.len(), 2);
    assert_eq!(project.durable_objects[0].class_name, "Counter");
    assert!(project.sqlite_classes.contains("Counter"));
    assert_eq!(project.services[0].service, "demo");
    assert_eq!(project.warnings.len(), 1);
    assert!(project.warnings[0].contains("KV namespaces"));
    assert!(project.warnings[0].contains("(KV)"));

    let text = project.to_capnp_text();
    assert!(text.contains("(name = \"GREETING\", text = \"hi\")"));
    assert!(text.contains("(name = \"LIMITS\", json = \"{\\\"max\\\":3}\")"));
    assert!(text.contains("(name = \"COUNTER\", durableObjectNamespace = \"Counter\")"));
    assert!(text.contains("uniqueKey = \"demo-Counter\", enableSql = true"));
    let root = fs::canonicalize(&dir).unwrap();
    assert_eq!(project.persist, Some(root.join(".workerd-state")));
    assert!(text.contains("disk = (path = \".workerd-state\", writable = true)"));
    assert!(text.contains("durableObjectStorage = (localDisk = \"demo-storage\")"));
    assert!(text.contains("address = \"*:1234\""));
    assert!(project.to_config().is_ok());
}

#[test]
fn wrangler_config_without_main_is_an_error() {
    let dir = temp_dir("nomain");
    fs::write(dir.join("wrangler.json"), r#"{ "name": "x" }"#).unwrap();
    let error = Project::from_wrangler_config(
        dir.join("wrangler.json").to_str().unwrap(),
        "2026-01-01",
        &Overrides::default(),
    )
    .unwrap_err();
    assert!(error.to_string().contains("\"main\" is required"));
}
