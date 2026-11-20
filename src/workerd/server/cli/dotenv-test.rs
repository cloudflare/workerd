// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The expected values are what the unmodified dotenv 16.3.1 `parse()` and dotenv-expand 12.0.2
// `expand()` give under Node, except where a case says it is not ported.

use std::cell::RefCell;

use super::*;

fn vars(pairs: &[(&str, &str)]) -> Vars {
    Vars(
        pairs
            .iter()
            .map(|(k, v)| ((*k).to_owned(), (*v).to_owned()))
            .collect(),
    )
}

#[test]
fn parse_as_dotenv() {
    let cases: &[(&str, &[(&str, &str)])] = &[
        ("export FOO=bar", &[("FOO", "bar")]),
        ("export FOO", &[]),
        ("FOO: bar", &[("FOO", "bar")]),
        ("FOO:bar", &[]),
        ("FOO = bar  ", &[("FOO", "bar")]),
        ("   FOO=bar", &[("FOO", "bar")]),
        ("FOO=bar baz", &[("FOO", "bar baz")]),
        ("FOO=a=b", &[("FOO", "a=b")]),
        ("FOO==bar", &[("FOO", "=bar")]),
        ("FOO=", &[("FOO", "")]),
        ("FOO=\"\"", &[("FOO", "")]),
        ("FOO", &[]),
        ("FOO='a\\nb'", &[("FOO", "a\\nb")]),
        ("FOO=\"a\\nb\"", &[("FOO", "a\nb")]),
        ("FOO=\"a\\rb\"", &[("FOO", "a\rb")]),
        ("FOO=`a\\nb`", &[("FOO", "a\\nb")]),
        ("FOO=a\\nb", &[("FOO", "a\\nb")]),
        ("FOO=\"a\\tb\"", &[("FOO", "a\\tb")]),
        ("FOO=\"say \\\"hi\\\"\"", &[("FOO", "say \\\"hi\\\"")]),
        ("FOO=\"a\\\\nb\"", &[("FOO", "a\\\nb")]),
        ("FOO=\"  padded  \"", &[("FOO", "  padded  ")]),
        ("FOO=  'x'  ", &[("FOO", "x")]),
        ("FOO=abc#def", &[("FOO", "abc")]),
        ("FOO=abc #def", &[("FOO", "abc")]),
        ("FOO=\"abc#def\" # c", &[("FOO", "abc#def")]),
        ("FOO=\"abc\" def", &[("FOO", "\"abc\" def")]),
        ("# FOO=bar\nBAR=1", &[("BAR", "1")]),
        ("   # FOO=bar", &[]),
        ("FOO=1\nFOO=2", &[("FOO", "2")]),
        ("FOO=\"l1\nl2\"\nBAR=x", &[("FOO", "l1\nl2"), ("BAR", "x")]),
        ("FOO='l1\nl2'", &[("FOO", "l1\nl2")]),
        ("FOO=`l1\nl2`", &[("FOO", "l1\nl2")]),
        ("FOO=\"abc\nBAR=x", &[("FOO", "\"abc"), ("BAR", "x")]),
        (
            "FOO=\"abc\nBAR=x\"\nBAZ=y",
            &[("FOO", "abc\nBAR=x"), ("BAZ", "y")],
        ),
        ("KEY=\n\"quoted\"", &[("KEY", "quoted")]),
        ("KEY=\nOTHER=x", &[("KEY", ""), ("OTHER", "x")]),
        ("KEY:\nOTHER=x", &[("KEY", "OTHER=x")]),
        (
            "FOO=a\r\nBAR=\"b\r\nc\"\r\n",
            &[("FOO", "a"), ("BAR", "b\nc")],
        ),
        ("FOO=a\rBAR=b", &[("FOO", "a"), ("BAR", "b")]),
        ("\u{FEFF}FOO=bar", &[("FOO", "bar")]),
        (
            "my.key-1=x\n1KEY=y\nKÉY=z",
            &[("my.key-1", "x"), ("1KEY", "y")],
        ),
        ("FOO=\"it's\"", &[("FOO", "it's")]),
        ("FOO='abc\"", &[("FOO", "'abc\"")]),
        ("export FOO: bar", &[("FOO", "bar")]),
        ("FOO\t=\tbar", &[("FOO", "bar")]),
        ("FOO=\"x\"   ", &[("FOO", "x")]),
    ];
    for (src, want) in cases {
        assert_eq!(parse(src), vars(want), "{src:?}");
    }
}

#[test]
fn expand_as_dotenv_expand() {
    type Case<'a> = (&'a str, &'a [(&'a str, &'a str)], &'a [(&'a str, &'a str)]);
    let cases: &[Case] = &[
        ("A=1\nB=$A", &[], &[("A", "1"), ("B", "1")]),
        ("B=$A\nA=1", &[], &[("B", ""), ("A", "1")]),
        ("A=1\nB=${A}x", &[], &[("A", "1"), ("B", "1x")]),
        ("A=1\nB=$Ax", &[], &[("A", "1"), ("B", "")]),
        ("B=${A:-def}", &[], &[("B", "def")]),
        ("B=${A-def}", &[], &[("B", "def")]),
        ("A=\nB=${A-def}", &[], &[("A", ""), ("B", "def")]),
        ("A=\nB=${A:-def}", &[], &[("A", ""), ("B", "def")]),
        ("A=1\nB=${A:+alt}", &[], &[("A", "1"), ("B", "alt")]),
        ("B=${A+alt}", &[], &[("B", "")]),
        ("B=${A:-hello world}", &[], &[("B", "hello world")]),
        ("C=c\nB=${A:-${C}}", &[], &[("C", "c"), ("B", "c")]),
        ("A=1\nB=\\$A", &[], &[("A", "1"), ("B", "$A")]),
        ("A=1\nB=\\${A}", &[], &[("A", "1"), ("B", "${A}")]),
        ("A=1\nB='$A'", &[], &[("A", "1"), ("B", "1")]),
        ("A=1\nB=\"$A\"", &[], &[("A", "1"), ("B", "1")]),
        ("PASSWORD=pa$word", &[], &[("PASSWORD", "pa")]),
        ("B=$HOME", &[], &[("B", "")]),
        ("B=$HOME", &[("HOME", "/h")], &[("B", "/h")]),
        ("A=f", &[("A", "p")], &[("A", "p")]),
        ("A=f", &[("A", "")], &[("A", "f")]),
        ("A=f\nB=$A", &[("A", "p")], &[("A", "p"), ("B", "p")]),
        ("A=f", &[("A", "x\\$y")], &[("A", "x$y")]),
        ("B=$P", &[("P", "$Q"), ("Q", "q")], &[("B", "q")]),
        ("A=$A", &[], &[("A", "")]),
        ("A=$A", &[("A", "p")], &[("A", "p")]),
        ("B=${MY-VAR}", &[], &[("B", "VAR")]),
        ("B=cost $5", &[], &[("B", "cost $5")]),
        ("B=abc$", &[], &[("B", "abc$")]),
        ("A=1\nB=$A-$A", &[], &[("A", "1"), ("B", "1-1")]),
        // Not ported; dotenv-expand gives "a", "a" and "m".
        ("B=$X", &[("X", "a$&b")], &[("B", "a$&b")]),
        ("B=$X", &[("X", "a$$b")], &[("B", "a$")]),
        ("MY=m\nB=${MYnullVAR}", &[], &[("MY", "m"), ("B", "")]),
    ];
    for (src, env, want) in cases {
        let parsed = parse(src);
        let out = expand(&parsed, vars(env));
        let got = parsed
            .0
            .iter()
            .map(|(k, _)| (k.clone(), out.get(k).unwrap().to_owned()));
        assert_eq!(Vars(got.collect()), vars(want), "{src:?} {env:?}");
    }
}

/// A fresh directory in the test's temp directory holding `files`; a name ending in `/` is an
/// empty directory.
fn project(files: &[(&str, &str)]) -> PathBuf {
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static COUNTER: AtomicUsize = AtomicUsize::new(0);
    let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    let dir = tmp.join(format!("workerd-dotenv-test-{}-{n}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    for (name, contents) in files {
        match name.strip_suffix('/') {
            Some(name) => fs::create_dir(dir.join(name)).unwrap(),
            None => fs::write(dir.join(name), contents).unwrap(),
        }
    }
    dir
}

/// The bindings for `dir` and the file names watched.
fn bindings(dir: &Path, env_files: &[String], dev: bool) -> Result<(Vars, Vec<String>), String> {
    let watched = RefCell::new(Vec::new());
    let watch = |path: &Path| {
        let name = path.file_name().unwrap().to_str().unwrap();
        watched.borrow_mut().push(name.to_owned());
        Ok(())
    };
    let vars = Sources { env_files, dev }.bindings(dir, &watch)?;
    Ok((vars, watched.into_inner()))
}

#[test]
fn dev_vars_alone_unexpanded() {
    let dir = project(&[(".dev.vars", "A=1\nB=$A"), (".env", "C=3")]);
    let want = (
        vars(&[("A", "1"), ("B", "$A")]),
        vec![".dev.vars".to_owned()],
    );
    assert_eq!(bindings(&dir, &[], true).unwrap(), want);
    let dir = project(&[(".dev.vars", ""), (".env", "C=3")]);
    assert_eq!(
        bindings(&dir, &[], true).unwrap(),
        (vars(&[]), vec![".dev.vars".to_owned()])
    );
    let dir = project(&[(".dev.vars/", "")]);
    assert!(bindings(&dir, &[], true).unwrap_err().contains(".dev.vars"));
}

#[test]
fn env_files_merged_then_expanded() {
    let dir = project(&[
        (".env", "FOO=bar\nBAZ=${FOO}\nP=$PATH"),
        (".env.local", "FOO=qux"),
    ]);
    let want = vars(&[("FOO", "qux"), ("BAZ", "qux"), ("P", "")]);
    let watched = vec![".env".to_owned(), ".env.local".to_owned()];
    assert_eq!(bindings(&dir, &[], true).unwrap(), (want, watched));
    let dir = project(&[(".env", "B=$A"), (".env.local", "A=1")]);
    let want = vars(&[("B", ""), ("A", "1")]);
    assert_eq!(bindings(&dir, &[], true).unwrap().0, want);
}

#[test]
fn env_file_flag_replaces_defaults() {
    let dir = project(&[
        (".dev.vars", "A=dev"),
        (".env", "A=env"),
        ("x.env", "A=x\nB=$A"),
    ]);
    let given = [dir.join("missing.env"), dir.join("x.env")].map(|p| p.display().to_string());
    let want = (vars(&[("A", "x"), ("B", "x")]), vec!["x.env".to_owned()]);
    assert_eq!(bindings(&dir, &given, false).unwrap(), want);
    assert_eq!(bindings(&dir, &given, true).unwrap(), want);
    assert_eq!(bindings(&dir, &[], false).unwrap(), (vars(&[]), Vec::new()));
}
