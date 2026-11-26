use super::*;

/// A parsed MIME type as `MimeType` would store it: lowercased, with names lowercased and the
/// first value of a name kept.
fn store(essence: Essence<'_>, params: Vec<(Vec<u8>, Vec<u8>)>) -> String {
    let mut kept: Vec<(Vec<u8>, Vec<u8>)> = Vec::new();
    for (name, value) in params {
        let name = name.to_ascii_lowercase();
        if !kept.iter().any(|(n, _)| *n == name) {
            kept.push((name, value));
        }
    }
    let mut out = format!(
        "{}/{}",
        String::from_utf8_lossy(essence.type_).to_ascii_lowercase(),
        String::from_utf8_lossy(essence.subtype).to_ascii_lowercase()
    );
    for (name, value) in kept {
        out.push_str(&format!(
            ";{}={}",
            String::from_utf8_lossy(&name),
            String::from_utf8_lossy(&value)
        ));
    }
    out
}

fn collect(
    run: impl FnOnce(
        &mut Vec<u8>,
        &mut dyn FnMut(&[u8], &[u8]) -> Result<(), ()>,
    ) -> Option<Essence<'static>>,
) -> Option<String> {
    let mut params = Vec::new();
    let essence = run(&mut Vec::new(), &mut |name, value| {
        params.push((name.to_vec(), value.to_vec()));
        Ok(())
    })?;
    Some(store(essence, params))
}

fn parse(input: &'static str) -> Option<String> {
    collect(|scratch, param| {
        let (essence, rest) = parse_essence(input.as_bytes())?;
        parse_params(rest, scratch, param).ok()?;
        Some(essence)
    })
}

fn extract(input: &'static str) -> Option<String> {
    collect(|scratch, param| super::extract(input.as_bytes(), scratch, param).ok()?)
}

#[test]
fn essence() {
    assert_eq!(parse("text/plain").as_deref(), Some("text/plain"));
    assert_eq!(parse(" \r\n\tTEXT/Plain \t").as_deref(), Some("text/plain"));
    assert_eq!(parse("text/plain ; a=b").as_deref(), Some("text/plain;a=b"));
    assert_eq!(parse("*/*").as_deref(), Some("*/*"));
    for invalid in [
        "",
        " ",
        "text",
        "text/",
        "/plain",
        "text /plain",
        "text/ plain",
        "te\u{e9}xt/plain",
        "text/pl@in",
        "text/;a=b",
    ] {
        assert_eq!(parse(invalid), None, "{invalid:?}");
    }
}

#[test]
fn params() {
    assert_eq!(parse("a/b;c=d;e=f").as_deref(), Some("a/b;c=d;e=f"));
    assert_eq!(parse("a/b;C=D;c=e").as_deref(), Some("a/b;c=D"));
    assert_eq!(parse("a/b; c = d").as_deref(), Some("a/b"));
    assert_eq!(parse("a/b; c= d ;e=f").as_deref(), Some("a/b;c= d;e=f"));
    assert_eq!(parse("a/b;;c;d=e").as_deref(), Some("a/b;d=e"));
    assert_eq!(parse("a/b;c=").as_deref(), Some("a/b"));
    assert_eq!(parse("a/b;c=;d=e").as_deref(), Some("a/b;d=e"));
    assert_eq!(parse("a/b;c d=e;f=g").as_deref(), Some("a/b;f=g"));
    assert_eq!(parse("a/b;c d=e").as_deref(), Some("a/b"));
    assert_eq!(parse("a/b;=e;f=g").as_deref(), Some("a/b;f=g"));
    assert_eq!(parse("a/b;c=d\x01;e=f").as_deref(), Some("a/b;e=f"));
    assert_eq!(parse("a/b;c=d e").as_deref(), Some("a/b;c=d e"));
    assert_eq!(parse("a/b;tail").as_deref(), Some("a/b"));
}

#[test]
fn quoted_values() {
    assert_eq!(parse("a/b;c=\"d;e\";f=g").as_deref(), Some("a/b;c=d;e;f=g"));
    assert_eq!(parse("a/b;c=\"\"").as_deref(), Some("a/b;c="));
    assert_eq!(
        parse("a/b;c=\"d\\\"e\\\\f\"").as_deref(),
        Some("a/b;c=d\"e\\f")
    );
    assert_eq!(
        parse("a/b;c=\"unterminated").as_deref(),
        Some("a/b;c=unterminated")
    );
    assert_eq!(parse("a/b;c=\"esc\\").as_deref(), Some("a/b;c=esc\\"));
    assert_eq!(
        parse("a/b;c=\"d\" junk;e=f").as_deref(),
        Some("a/b;c=d;e=f")
    );
    assert_eq!(parse("a/b;c=\"d\" junk").as_deref(), Some("a/b;c=d"));
    assert_eq!(parse("a/b;c=\"\\\u{1}\";e=f").as_deref(), Some("a/b;e=f"));
}

#[test]
fn quoted_string() {
    let mut scratch = Vec::new();
    assert_eq!(
        collect_quoted_string(b"abc\"rest", &mut scratch),
        (&b"abc"[..], &b"rest"[..])
    );
    assert_eq!(
        collect_quoted_string(b"abc", &mut scratch),
        (&b"abc"[..], &b""[..])
    );
    assert_eq!(
        collect_quoted_string(b"a\\bc\\\"d\"rest", &mut scratch),
        (&b"abc\"d"[..], &b"rest"[..])
    );
    assert_eq!(
        collect_quoted_string(b"a\\", &mut scratch),
        (&b"a\\"[..], &b""[..])
    );
}

#[test]
fn params_stop_at_first_error() {
    let mut seen = 0;
    let result = parse_params(b"a=1;b=2;c=3", &mut Vec::new(), |_, _| {
        seen += 1;
        if seen == 2 { Err("stop") } else { Ok(()) }
    });
    assert_eq!(result, Err("stop"));
    assert_eq!(seen, 2);
}

#[test]
fn extraction() {
    assert_eq!(
        extract("text/html;charset=utf-8").as_deref(),
        Some("text/html;charset=utf-8")
    );
    assert_eq!(extract("").as_deref(), None);
    assert_eq!(extract("bogus").as_deref(), None);
    assert_eq!(extract("text/html, bogus").as_deref(), Some("text/html"));
    assert_eq!(extract("text/html, */*").as_deref(), Some("text/html"));
    assert_eq!(
        extract("text/plain, text/html").as_deref(),
        Some("text/html")
    );
    assert_eq!(extract("text/html,").as_deref(), Some("text/html"));
    assert_eq!(
        extract("text/html;charset=gbk, text/html").as_deref(),
        Some("text/html;charset=gbk")
    );
    assert_eq!(
        extract("text/html;charset=gbk;a=b, TEXT/HTML;x=y").as_deref(),
        Some("text/html;x=y;charset=gbk")
    );
    assert_eq!(
        extract("text/html;charset=gbk, text/plain, text/html").as_deref(),
        Some("text/html")
    );
    assert_eq!(
        extract("text/html;charset=gbk, bogus, */*, text/html").as_deref(),
        Some("text/html;charset=gbk")
    );
    assert_eq!(
        extract("text/html;charset=gbk, text/html;charset=latin1").as_deref(),
        Some("text/html;charset=latin1")
    );
    assert_eq!(
        extract("text/html;charset=gbk, text/html, text/html, bogus").as_deref(),
        Some("text/html;charset=gbk")
    );
    assert_eq!(
        extract("text/html;charset=\"g\\bk\", text/html;charset=\x01").as_deref(),
        Some("text/html;charset=gbk")
    );
    assert_eq!(
        extract("text/html;Charset=a;charset=b, text/html").as_deref(),
        Some("text/html;charset=a")
    );
}

#[test]
fn separators() {
    assert_eq!(find_next_separator(b"a,b"), Some(1));
    assert_eq!(find_next_separator(b"a\\,b,c"), Some(4));
    assert_eq!(find_next_separator(b"\"a,b\",c"), Some(5));
    assert_eq!(find_next_separator(b"\"a\\\",b\",c"), Some(7));
    assert_eq!(find_next_separator(b"\\\"a,b"), Some(3));
    assert_eq!(find_next_separator(b"\"a,b"), None);
    assert_eq!(find_next_separator(b",a"), Some(0));
    assert_eq!(extract("a/b;c=\"x,y\",d/e").as_deref(), Some("d/e"));
    assert_eq!(extract("a/b;c=\"x,y\"").as_deref(), Some("a/b;c=x,y"));
}
