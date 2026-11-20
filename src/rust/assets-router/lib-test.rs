use super::*;

/// The config of `json`, an `Options`.
pub fn from_json(
    json: &str,
    has_user_worker: bool,
    navigation: bool,
    warn: &mut dyn FnMut(String),
) -> Result<Config, String> {
    let options = serde_json::from_str(json).map_err(|e| e.to_string())?;
    let deployment = Deployment {
        has_user_worker,
        navigation_prefers_asset_serving: navigation,
    };
    Config::new(options, deployment, warn)
}

fn parse(json: &str) -> Result<Config, String> {
    from_json(json, true, false, &mut |w| panic!("{w}"))
}

#[test]
fn defaults() {
    let config = parse("{}").unwrap();
    let routing = &config.routing;
    assert!(routing.has_user_worker);
    assert!(!routing.invoke_user_worker_ahead_of_assets);
    assert_eq!(routing.intent, intent::IntentConfig::default());
    assert!(routing.base_path.is_root());
    assert!(!config.debug);
    assert!(config.ignored.ignored("/_headers"));
}

#[test]
fn every_field_null_is_the_default() {
    let config = parse(
        r#"{"html_handling": null, "not_found_handling": null, "base_path": null, "run_worker_first": null,
            "headers": null, "redirects": null, "ignore": null, "debug": null}"#,
    );
    assert_eq!(
        config.unwrap().routing.intent,
        intent::IntentConfig::default()
    );
}

#[test]
fn fields() {
    let config = parse(
        r#"{"html_handling": "drop-trailing-slash", "not_found_handling": "404-page",
            "base_path": "/sub", "run_worker_first": ["/api/*", "!/api/x"],
            "headers": "/*\n  X: y", "redirects": "/a /b", "ignore": "x", "debug": true}"#,
    )
    .unwrap();
    let routing = &config.routing;
    assert_eq!(
        routing.intent.html_handling,
        intent::HtmlHandling::DropTrailingSlash
    );
    assert_eq!(
        routing.intent.not_found_handling,
        intent::NotFoundHandling::NotFoundPage
    );
    assert!(routing.intent.has_static_routing);
    assert_eq!(routing.base_path.as_str(), "/sub/");
    assert_eq!(routing.rule_route("/api/y"), decide::RuleRoute::UserWorker);
    assert_eq!(routing.rule_route("/api/x"), decide::RuleRoute::AssetWorker);
    assert_eq!(routing.rule_route("/b"), decide::RuleRoute::Undecided);
    assert!(config.debug);
    assert!(config.ignored.ignored("/x"));

    let all = parse(r#"{"run_worker_first": true}"#).unwrap();
    assert_eq!(all.routing.rule_route("/b"), decide::RuleRoute::UserWorker);
    assert!(!all.routing.intent.has_static_routing);
}

#[test]
fn errors() {
    for (json, error) in [
        (
            r#"{"run_worker_first": ["api/*"]}"#,
            "Invalid routes in `run_worker_first`:\n'api/*': rules must start with '/' or '!/'",
        ),
        (
            r#"{"base_path": "https://x"}"#,
            "Invalid assets base_path: The value must be a pathname, not a URL.",
        ),
    ] {
        assert_eq!(parse(json).err().unwrap(), error);
    }
    assert!(parse(r#"{"html_handling": "sometimes"}"#).is_err());
    assert!(parse("[").is_err());
}

#[test]
fn rule_file_warnings() {
    let mut warnings = Vec::new();
    let json = r#"{"headers": "nope", "redirects": "nope"}"#;
    from_json(json, false, false, &mut |w| warnings.push(w)).unwrap();
    assert_eq!(warnings.len(), 2);
    assert!(warnings[0].starts_with("Found 1 invalid redirect rule(s):"));
    assert!(warnings[1].starts_with("Found 1 invalid header rule(s):"));
}
