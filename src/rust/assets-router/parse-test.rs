// Upstream: workers-shared `utils/tests/parseHeaders.*.test.ts`, `parseRedirects.*.test.ts` and
// `parseStaticRouting.test.ts`.

use super::*;

type Rule = (String, Vec<(String, String)>, Vec<String>);

fn headers(input: &str) -> (Vec<Rule>, Vec<Invalid>) {
    let (rules, invalid) = parse_headers(input, MAX_HEADER_RULES);
    (
        rules
            .into_iter()
            .map(|(path, r)| (path, r.set, r.unset))
            .collect(),
        invalid,
    )
}

fn rule(path: &str, headers: &[(&str, &str)], unset: &[&str]) -> Rule {
    let headers = headers
        .iter()
        .map(|(n, v)| ((*n).into(), (*v).into()))
        .collect();
    (
        path.into(),
        headers,
        unset.iter().map(|&u| u.into()).collect(),
    )
}

fn redirects(input: &str) -> (Vec<(String, usize, Redirect)>, Vec<Invalid>) {
    let (max_static, max_dynamic) = (MAX_STATIC_REDIRECT_RULES, MAX_DYNAMIC_REDIRECT_RULES);
    parse_redirects(
        input,
        HtmlHandling::AutoTrailingSlash,
        max_static,
        max_dynamic,
    )
}

fn redirect(from: &str, to: &str, status: u16, line_number: usize) -> (String, usize, Redirect) {
    (
        from.into(),
        line_number,
        Redirect {
            status,
            to: to.into(),
        },
    )
}

fn invalid(line: &str, line_number: usize, message: &str) -> Invalid {
    Invalid::at(line, line_number, message)
}

#[test]
fn headers_rules() {
    let input = "
  # This is a comment
                /a
    Name         :           Value
  /b
    Set-Cookie: test=cookie; expires=never
    Set-Cookie: another=cookie; magic!
    !Name: Value
  ! Place
";
    assert_eq!(
        headers(input),
        (
            vec![
                rule("/a", &[("name", "Value")], &[]),
                rule(
                    "/b",
                    &[
                        (
                            "set-cookie",
                            "test=cookie; expires=never, another=cookie; magic!"
                        ),
                        ("!name", "Value")
                    ],
                    &["Place"]
                ),
            ],
            vec![]
        )
    );
}

#[test]
fn headers_hosts() {
    let input = "
  https://example.com
    a: a
  https://example.com/blog
    c: c
  https://:subdomain.example.*/path
    e:e
";
    assert_eq!(
        headers(input).0,
        [
            rule("https://example.com/", &[("a", "a")], &[]),
            rule("https://example.com/blog", &[("c", "c")], &[]),
            rule("https://:subdomain.example.*/path", &[("e", "e")], &[]),
        ]
    );
}

#[test]
fn headers_reject_names_with_non_token_characters() {
    let (rules, bad) = headers("/a\n  X@Foo: bar\n  Foo/Bar: baz\n  Ok: yes");
    assert_eq!(rules, [rule("/a", &[("ok", "yes")], &[])]);
    let message = "Header name can only include token characters";
    assert_eq!(
        bad,
        [
            invalid("X@Foo: bar", 2, message),
            invalid("Foo/Bar: baz", 3, message)
        ]
    );
}

#[test]
fn headers_invalid_lines() {
    let input = "
    c
    Access-Control-Allow-Origin: *
    /b
      I'm invalid!
      !x-content-type
      But: I'm okay!
      ! Content-Type: application/json
    /c
    /d
      :
      test:
  ";
    assert_eq!(
        headers(input),
        (
            vec![rule("/b", &[("but", "I'm okay!")], &[])],
            vec![
                invalid(
                    "c",
                    2,
                    "Expected a path beginning with at least one forward-slash"
                ),
                invalid(
                    "Access-Control-Allow-Origin: *",
                    3,
                    "Path should come before header (access-control-allow-origin: *)"
                ),
                invalid(
                    "I'm invalid!",
                    5,
                    "Expected a colon-separated header pair (e.g. name: value)"
                ),
                invalid(
                    "!x-content-type",
                    6,
                    "Expected a colon-separated header pair (e.g. name: value)"
                ),
                invalid(
                    "! Content-Type: application/json",
                    8,
                    "Header name cannot include spaces"
                ),
                invalid("/c", 10, "No headers specified"),
                invalid(":", 11, "No header name specified"),
                invalid("test:", 12, "No header value specified"),
                Invalid {
                    line: Some("/d".into()),
                    line_number: None,
                    message: "No headers specified".into(),
                },
            ]
        )
    );
}

#[test]
fn headers_invalid_paths() {
    let input = "
  /some page with spaces
    valid: yup
  /://so;`me
    valid: yup
  /nons:/&@%+~{}ense
    valid: yup
  http://nah.com/blog
    invalid: things
  https://nah.com:8080
    invalid: also
  //yeah.com/blog
    valid: things
  /blog/*/posts/*
    x: y
  /blog/*/:splat
    x: y
";
    let (rules, invalid_) = headers(input);
    let paths: Vec<_> = rules.iter().map(|r| r.0.as_str()).collect();
    assert_eq!(
        paths,
        [
            "/some%20page%20with%20spaces",
            "/://so;%60me",
            "/nons:/&@%+~%7B%7Dense",
            "//yeah.com/blog"
        ]
    );
    assert_eq!(
        invalid_,
        [
            invalid(
                "http://nah.com/blog",
                8,
                "URLs should either be relative (e.g. begin with a forward-slash), or use HTTPS \
                 (e.g. begin with \"https://\")."
            ),
            invalid(
                "https://nah.com:8080",
                10,
                "Specifying ports is not supported. Skipping absolute URL https://nah.com:8080."
            ),
            invalid(
                "/blog/*/posts/*",
                14,
                "Only one wildcard is allowed per rule. Use a named placeholder (e.g. :project) \
                 instead. Skipping /blog/*/posts/*."
            ),
            invalid(
                "/blog/*/:splat",
                16,
                "Cannot combine a wildcard * with a :splat placeholder because wildcards are \
                 converted to :splat at runtime. Skipping /blog/*/:splat."
            ),
        ]
    );
}

#[test]
fn headers_limits() {
    let huge = format!("/b\n{}: b", "a".repeat(2001));
    let (rules, invalid_) = headers(&format!("/a\n  Name: Value\n{}\n{huge}", "#".repeat(2001)));
    assert_eq!(rules, [rule("/a", &[("name", "Value")], &[])]);
    assert_eq!(
        invalid_[0],
        Invalid::new("Ignoring line 5 as it exceeds the maximum allowed length of 2000.".into())
    );

    let many: Vec<_> = (0..150).map(|i| format!("/a/{i}\nx-index: {i}")).collect();
    let (rules, invalid_) = headers(&format!("\n{}\n# end\n", many.join("\n")));
    assert_eq!(rules.len(), 101);
    assert_eq!(
        invalid_,
        [Invalid::new(
            "Maximum number of rules supported is 100. Skipping remaining 100 lines of file."
                .into()
        )]
    );
}

#[test]
fn redirects_rules() {
    let input = "
  # This is a comment
  /a /b 301
  /c /d
  /e /f?query=string 307 # comment
  /g?this=rejected /h 308
  /ext https://some.domain:1234/route?q=string#anchor
  /frag /b##blah-1 #
  /proxy /b 200
  /cat https://cat.com:12345 302
  /* /foo 200
";
    assert_eq!(
        redirects(input),
        (
            vec![
                redirect("/a", "/b", 301, 3),
                redirect("/c", "/d", 302, 4),
                redirect("/e", "/f?query=string", 307, 5),
                redirect("/g", "/h", 308, 6),
                redirect(
                    "/ext",
                    "https://some.domain:1234/route?q=string#anchor",
                    302,
                    7
                ),
                redirect("/frag", "/b##blah-1", 302, 8),
                redirect("/proxy", "/b", 200, 9),
                redirect("/cat", "https://cat.com:12345/", 302, 10),
                redirect("/*", "/foo", 200, 11),
            ],
            vec![]
        )
    );
}

#[test]
fn redirects_invalid_lines() {
    let input = "
    /c
    /d /e 302 !important
    /f /g 418
    /a /b
    /a /c
    https://yeah.com https://nah.com
    /x https://example.com/b 200
    /* /index.html 200
    / /index
    /some%20page /somewhere%20else
    /://so;`me /nons:/&@%+~{}ense
    yeah.com https://nah.com
  ";
    let (rules, invalid_) = redirects(input);
    assert_eq!(
        rules,
        [
            redirect("/a", "/b", 302, 5),
            redirect("/some%20page", "/somewhere%20else", 302, 11),
            redirect("/://so;%60me", "/nons:/&@%+~%7B%7Dense", 302, 12),
            redirect("/yeah.com", "https://nah.com/", 302, 13),
        ]
    );
    let looping = "Infinite loop detected in this rule and has been ignored. This will cause a \
                   redirect to strip `.html` or `/index` and end up triggering this rule again. \
                   Please fix or remove this rule to silence this warning.";
    assert_eq!(
        invalid_,
        [
            invalid(
                "/c",
                2,
                "Expected exactly 2 or 3 whitespace-separated tokens. Got 1."
            ),
            invalid(
                "/d /e 302 !important",
                3,
                "Expected exactly 2 or 3 whitespace-separated tokens. Got 4."
            ),
            invalid(
                "/f /g 418",
                4,
                "Valid status codes are 200, 301, 302 (default), 303, 307, or 308. Got 418."
            ),
            invalid("/a /c", 6, "Ignoring duplicate rule for path /a."),
            invalid(
                "https://yeah.com https://nah.com",
                7,
                "Only relative URLs are allowed. Skipping absolute URL https://yeah.com."
            ),
            invalid(
                "/x https://example.com/b 200",
                8,
                "Proxy (200) redirects can only point to relative paths. Got \
                 https://example.com/b"
            ),
            invalid("/* /index.html 200", 9, looping),
            invalid("/ /index", 10, looping),
        ]
    );
    // Without HTML handling `/` to `/index` does not loop.
    let (rules, _) = parse_redirects("/ /index", HtmlHandling::None, 1, 1);
    assert_eq!(rules, [redirect("/", "/index", 302, 1)]);
}

#[test]
fn redirects_limits() {
    let dynamic: Vec<_> = (0..150)
        .map(|i| format!("/a/{i}/* /b/{i}/:splat"))
        .collect();
    let (rules, invalid_) = redirects(&format!("\n{}\n", dynamic.join("\n")));
    assert_eq!(rules.len(), 100);
    assert_eq!(
        invalid_,
        [Invalid::new(
            "Maximum number of dynamic rules supported is 100. Skipping remaining 51 lines of \
             file."
                .into()
        )]
    );
    let lines: Vec<_> = (0..3).map(|i| format!("/a/{i} /b/{i}")).collect();
    let (rules, invalid_) = parse_redirects(&lines.join("\n"), HtmlHandling::None, 2, 1);
    assert_eq!(rules.len(), 2);
    assert_eq!(
        invalid_,
        [Invalid::new(
            "Maximum number of static rules supported is 2. Skipping line.".into()
        )]
    );
}

#[test]
fn constructed_rules_and_warnings() {
    let mut warnings = Vec::new();
    let text = "/a /b 301\n/c/* /d/:splat\n/e /f\nnope\n";
    let (static_rules, dynamic_rules) =
        super::redirects(text, HtmlHandling::None, &mut |w| warnings.push(w));
    let to = |r: &Redirect| (r.status, r.to.clone());
    assert_eq!(static_rules.len(), 1);
    assert_eq!(static_rules[0].0, "/a");
    // After the first dynamic rule, every rule is dynamic.
    assert_eq!(
        dynamic_rules
            .iter()
            .map(|(f, r)| (f.as_str(), to(r)))
            .collect::<Vec<_>>(),
        [
            ("/c/*", (302, "/d/:splat".into())),
            ("/e", (302, "/f".into()))
        ]
    );
    assert_eq!(
        warnings,
        [
            "Found 1 invalid redirect rule(s):\n  Expected exactly 2 or 3 whitespace-separated \
          tokens. Got 1.\n    at _redirects:4 | nope"
        ]
    );

    let mut warnings = Vec::new();
    let rules = super::headers("/a\n  x: 1\n/a\n  y: 2\n! z\n", &mut |w| warnings.push(w));
    assert_eq!(rules.len(), 1);
    assert_eq!(rules[0].1.set, [("y".to_owned(), "2".to_owned())]);
    assert_eq!(rules[0].1.unset, ["z"]);
    assert_eq!(warnings, [] as [String; 0]);
}

#[test]
fn static_routing_rules() {
    let rules =
        |rules: &[&str]| static_routing(&rules.iter().map(|&r| r.into()).collect::<Vec<_>>());
    assert_eq!(
        rules(&["/api/*", "/oauth/callback", "!/api/assets/*"]),
        Ok((
            vec!["/api/*".into(), "/oauth/callback".into()],
            vec!["/api/assets/*".into()]
        ))
    );
    assert_eq!(
        rules(&[]),
        Err("No `run_worker_first` rules were provided; must provide at least 1 rule.".into())
    );
    assert_eq!(
        rules(&["!/assets"]),
        Err(
            "Only negative `run_worker_first` rules were provided; must provide at least 1 \
             non-negative rule"
                .into()
        )
    );
    assert_eq!(
        rules(&["api/*", "!asset"]),
        Err(
            "Invalid routes in `run_worker_first`:\n'api/*': rules must start with '/' or \
             '!/'\n'!asset': negative rules must start with '!/'"
                .into()
        )
    );
    assert_eq!(
        rules(&[
            "/api/*",
            "/oauth/callback",
            "/api/some/route",
            "/oauth/callback"
        ]),
        Err(
            "Invalid routes in `run_worker_first`:\n'/api/some/route': rule '/api/*' makes it \
             redundant\n'/oauth/callback': rule is a duplicate; rules must be unique"
                .into()
        )
    );
    let long = format!("/api/{}", "a".repeat(130));
    assert_eq!(
        rules(&[&long]),
        Err(format!(
            "Invalid routes in `run_worker_first`:\n'{long}': all rules must be less than 100 \
             characters in length"
        ))
    );
    let many: Vec<String> = (0..99)
        .map(|i| format!("/rule/{i}"))
        .chain((0..5).map(|_| "/rule/0".into()))
        .collect();
    assert_eq!(
        static_routing(&many),
        Err(
            "Too many `run_worker_first` rules were provided; 104 rules provided (99 \
             distinct) exceeds max of 100."
                .into()
        )
    );
}

#[test]
fn skips_rules_that_cannot_compile() {
    let mut warnings = Vec::new();
    let rules = super::headers("/:a/:a\n  X: 1\n/ok\n  X: 2\n", &mut |w| warnings.push(w));
    assert_eq!(rules.len(), 1);
    assert_eq!(rules[0].0, "/ok");
    let (_, dynamic) = super::redirects("/:a/:a /b\n/c/* /d\n", HtmlHandling::None, &mut |w| {
        warnings.push(w);
    });
    assert_eq!(dynamic.len(), 1);
    assert_eq!(warnings.len(), 2);
}
