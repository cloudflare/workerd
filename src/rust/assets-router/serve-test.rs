// Upstream: asset-worker/tests/handler.test.ts, the headers and status of a served asset.

use url::Url;

use super::*;

fn config(json: &str) -> Config {
    crate::tests::from_json(json, false, false, &mut |w| panic!("{w}")).unwrap()
}

fn asset(path: &str, status: u16, not_found: bool) -> Asset {
    Asset {
        path: path.into(),
        etag: "e".into(),
        status,
        not_found,
    }
}

fn serve(config: &Config, method: Method, conditions: Conditions<'_>, asset: &Asset) -> Served {
    let url = Url::parse(&format!("https://example.com{}", asset.path)).unwrap();
    let request = Request {
        url: &url,
        method,
        sec_fetch_mode: None,
        sec_fetch_dest: None,
    };
    config.serve(&request, conditions, asset, "1-2")
}

fn header(name: &str, value: &str) -> ResponseHeader {
    ResponseHeader {
        name: name.into(),
        value: value.into(),
    }
}

#[test]
fn content_types() {
    assert_eq!(
        content_type("/a/b.HTML").as_deref(),
        Some("text/html; charset=utf-8")
    );
    assert_eq!(content_type("/x.wasm").as_deref(), Some("application/wasm"));
    assert_eq!(content_type("/x.unknown"), None);
    assert_eq!(content_type("/html"), None);
}

#[test]
fn served_headers() {
    let config = config("{}");
    let get = Conditions::default();
    assert_eq!(
        serve(&config, Method::Get, get, &asset("/a.css", 200, false)),
        Served {
            status: 200,
            headers: vec![
                header("ETag", "\"1-2\""),
                header("Content-Type", "text/css; charset=utf-8"),
                header("Cache-Control", "public, max-age=0, must-revalidate"),
            ],
            body: true,
        }
    );
    let private = Conditions {
        private: true,
        ..get
    };
    let served = serve(
        &config,
        Method::Head,
        private,
        &asset("/404.html", 404, true),
    );
    assert_eq!(served.status, 404);
    assert!(!served.body);
    assert_eq!(served.headers.len(), 2);
}

#[test]
fn not_modified() {
    let config = config("{}");
    for (if_none_match, status) in [
        ("\"1-2\"", 304),
        ("W/\"1-2\"", 304),
        ("\"1-3\"", 200),
        ("1-2", 200),
    ] {
        let conditions = Conditions {
            if_none_match: Some(if_none_match),
            private: false,
        };
        let served = serve(&config, Method::Get, conditions, &asset("/a", 200, false));
        assert_eq!(served.status, status, "{if_none_match}");
        assert_eq!(served.body, status == 200);
    }
}

#[test]
fn debug_header_for_a_navigation_not_found_answered() {
    let header = "X-Mf-Additional-Response-Log";
    let has = |json, navigation, not_found| {
        let config =
            crate::tests::from_json(json, false, navigation, &mut |w| panic!("{w}")).unwrap();
        let served = serve(
            &config,
            Method::Get,
            Conditions::default(),
            &asset("/index.html", 200, not_found),
        );
        served.headers.iter().any(|h| h.name == header)
    };
    let debug = r#"{"debug": true}"#;
    assert!(has(debug, true, true));
    assert!(!has(debug, true, false));
    assert!(!has(debug, false, true));
    assert!(!has("{}", true, true));
}

#[test]
fn custom_headers_apply() {
    let config = config(r#"{"headers": "/*\n  X-Site: :splat\n  ! Cache-Control\n  ETag: x"}"#);
    let served = serve(
        &config,
        Method::Get,
        Conditions::default(),
        &asset("/a.txt", 200, false),
    );
    assert_eq!(
        served.headers,
        [
            header("Content-Type", "text/plain; charset=utf-8"),
            header("x-site", "a.txt"),
            header("etag", "x"),
        ]
    );
}
