//! `.env` files as wrangler reads them (workers-sdk ca55031d). [`parse`] is dotenv 16.3.1's
//! `parse()` and [`expand`] is dotenv-expand 12.0.2's `expand()`, both `lib/main.js`, the versions
//! wrangler bundles. Not ported: dotenv's `DOTENV_KEY` vault, and two accidents of
//! dotenv-expand's JavaScript: `$&`, `$$` and the like in a substituted value
//! (`String.prototype.replace`), and an expression without an operator split on the string
//! `"null"` (`split(null)`).
//!
//! wrangler reads them twice. [`load_into_process`] is its own environment
//! (`wrangler/src/index.ts`); [`Sources::bindings`] is a Worker's string bindings
//! (`wrangler/src/dev/dev-vars.ts`). Unlike wrangler, `--env-file` resolves against the current
//! directory in both, and `workerd serve` reads no file without `--env-file`.

#![allow(
    unsafe_code,
    reason = "std::env::set_var, which is safe while no other thread runs"
)]

use std::collections::HashSet;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::Path;
use std::path::PathBuf;
use std::sync::LazyLock;
use std::sync::OnceLock;

use regex::Regex;

/// JavaScript's `\s`, which `String.prototype.trim` also removes.
const JS_SPACE: &str = concat!(
    r"[\t\n\x0B\x0C\r \x{A0}\x{1680}\x{2000}-\x{200A}",
    r"\x{2028}\x{2029}\x{202F}\x{205F}\x{3000}\x{FEFF}]",
);

/// dotenv's `LINE`, with JavaScript's `\s`, `\w` and `.`.
static LINE: LazyLock<Regex> = LazyLock::new(|| {
    let line = concat!(
        r"(?m)^\s*(?:export\s+)?([A-Za-z0-9_.-]+)(?:\s*=\s*?|:\s+?)",
        r#"(\s*'(?:\\'|[^'])*'|\s*"(?:\\"|[^"])*"|\s*`(?:\\`|[^`])*`|[^#\r\n]+)?"#,
        r"\s*(?:#[^\n\r\x{2028}\x{2029}]*)?$",
    );
    regex(&line.replace(r"\s", JS_SPACE))
});

/// dotenv-expand's reference, `(?<!\\)\${([^{}]+)}|(?<!\\)\$([A-Za-z_][A-Za-z0-9_]*)`, with the
/// look-behind as a character before it.
static REFERENCE: LazyLock<Regex> =
    LazyLock::new(|| regex(r"(?:^|[^\\])(\$\{([^{}]+)\}|\$([A-Za-z_][A-Za-z0-9_]*))"));

static OPERATOR: LazyLock<Regex> = LazyLock::new(|| regex(r":\+|\+|:-|-"));

#[expect(
    clippy::unwrap_used,
    reason = "a constant pattern, which the tests compile"
)]
fn regex(pattern: &str) -> Regex {
    Regex::new(pattern).unwrap()
}

/// Variables in the order each name first appeared, as in a JavaScript object.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Vars(pub Vec<(String, String)>);

impl Vars {
    fn get(&self, key: &str) -> Option<&str> {
        self.0
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }

    fn set(&mut self, key: &str, value: String) {
        match self.0.iter_mut().find(|(k, _)| k == key) {
            Some((_, v)) => *v = value,
            None => self.0.push((key.to_owned(), value)),
        }
    }
}

const fn is_js_space(c: char) -> bool {
    matches!(
        c,
        '\t' | '\n' | '\x0B' | '\x0C' | '\r' | ' ' | '\u{A0}' | '\u{1680}'
    ) || matches!(c, '\u{2000}'..='\u{200A}')
        || matches!(
            c,
            '\u{2028}' | '\u{2029}' | '\u{202F}' | '\u{205F}' | '\u{3000}' | '\u{FEFF}'
        )
}

/// dotenv's `parse()`: the variables of a `.env` file.
pub fn parse(src: &str) -> Vars {
    let src = src.replace("\r\n", "\n").replace('\r', "\n");
    let mut vars = Vars::default();
    for c in LINE.captures_iter(&src) {
        let value = c
            .get(2)
            .map_or("", |m| m.as_str())
            .trim_matches(is_js_space);
        let quoted =
            value.len() > 1 && value.starts_with(['\'', '"', '`']) && value.ends_with(&value[..1]);
        let mut stripped = if quoted {
            &value[1..value.len() - 1]
        } else {
            value
        }
        .to_owned();
        if value.starts_with('"') {
            stripped = stripped.replace(r"\n", "\n").replace(r"\r", "\r");
        }
        vars.set(&c[1], stripped);
    }
    vars
}

/// dotenv-expand's `expand()`: each of `parsed`'s values in turn expanded against `env` and the
/// values before it, or `env`'s own value where that is not empty and differs; then each added to
/// `env`, which is returned.
pub fn expand(parsed: &Vars, mut env: Vars) -> Vars {
    let mut running = Vars::default();
    for (key, value) in &parsed.0 {
        let value = match env.get(key) {
            Some(own) if !own.is_empty() && own != *value => own.to_owned(),
            _ => expand_value(value, &env, &running),
        };
        running.set(key, value.replace(r"\$", "$"));
    }
    for (key, value) in running.0 {
        env.set(&key, value);
    }
    env
}

/// dotenv-expand's `expandValue()`.
fn expand_value(value: &str, env: &Vars, running: &Vars) -> String {
    let lookup = |key: &str| env.get(key).or_else(|| running.get(key)).unwrap_or("");
    let mut result = value.to_owned();
    let mut seen = HashSet::new();
    while let Some(c) = REFERENCE.captures(&result) {
        let template = c[1].to_owned();
        let Some(expression) = c.get(2).or_else(|| c.get(3)) else {
            break;
        };
        let expression = expression.as_str().to_owned();
        seen.insert(result.clone());
        let op = OPERATOR.find(&expression);
        let key = op.map_or(&expression[..], |m| &expression[..m.start()]);
        let rest = op.map_or("", |m| &expression[m.end()..]);
        let with = match op.map(|m| m.as_str()) {
            Some(":+" | "+") if lookup(key).is_empty() => "",
            Some(":+" | "+") => rest,
            _ if lookup(key).is_empty() || seen.contains(lookup(key)) => rest,
            _ => lookup(key),
        };
        result = result.replacen(&template, with, 1);
        if running.get(key) == Some(&result[..]) {
            break;
        }
    }
    result
}

/// The `.env` files at `paths` merged, a later file winning, as wrangler's `loadDotEnv` merges
/// them, and the files read; one that cannot be read is skipped.
fn read_all(paths: &[PathBuf]) -> (Vars, Vec<PathBuf>) {
    let mut vars = Vars::default();
    let mut read = Vec::new();
    for path in paths {
        let Ok(bytes) = fs::read(path) else { continue };
        for (k, v) in parse(&String::from_utf8_lossy(&bytes)).0 {
            vars.set(&k, v);
        }
        read.push(path.clone());
    }
    (vars, read)
}

/// The `--env-file` files, else `.env` and `.env.local` in `dir`, as absolute paths.
fn named(given: &[String], dir: &Path) -> Vec<PathBuf> {
    let paths: Vec<PathBuf> = if given.is_empty() {
        vec![dir.join(".env"), dir.join(".env.local")]
    } else {
        given.iter().map(PathBuf::from).collect()
    };
    paths
        .iter()
        .filter_map(|p| std::path::absolute(p).ok())
        .collect()
}

static ORIGINAL: OnceLock<Vec<(OsString, OsString)>> = OnceLock::new();

/// This process's environment before [`load_into_process`] changed it, for `--watch`'s reload to
/// start again from.
pub fn original_environment() -> Option<&'static [(OsString, OsString)]> {
    ORIGINAL.get().map(Vec::as_slice)
}

/// Adds the variables of the `--env-file` files, else under `workerd dev` of `.env` and
/// `.env.local`, to this process's environment, as wrangler does to its own: a variable already
/// set to a non-empty value keeps it, and values may refer to the environment. Returns the files
/// read. Call it before starting any thread.
pub fn load_into_process(given: &[String], dev: bool) -> Vec<PathBuf> {
    if given.is_empty() && !dev {
        return Vec::new();
    }
    let environment = ORIGINAL.get_or_init(|| std::env::vars_os().collect());
    let (parsed, read) = read_all(&named(given, Path::new("")));
    let own = environment
        .iter()
        .filter_map(|(k, v)| Some((k.to_str()?, v.to_str()?)));
    let own = Vars(own.map(|(k, v)| (k.to_owned(), v.to_owned())).collect());
    let merged = expand(&parsed, own);
    for (key, _) in &parsed.0 {
        let value = merged.get(key).unwrap_or_default();
        if !value.contains('\0') {
            // SAFETY: no other thread runs yet to read the environment.
            unsafe { std::env::set_var(key, value) };
        }
    }
    read
}

/// Where a wrangler config's Worker gets string bindings besides `vars`, as `wrangler dev` does:
/// the `--env-file` files, else under `workerd dev` the config's `.dev.vars`, else its `.env` and
/// `.env.local`.
#[derive(Clone, Copy, Default)]
pub struct Sources<'a> {
    pub env_files: &'a [String],
    pub dev: bool,
}

impl Sources<'_> {
    /// The bindings for the config in `dir`, watching each file read. Names each file read, as
    /// wrangler does. A `.dev.vars` is used alone, even empty, and not expanded; the `.env` files
    /// are expanded without the process's environment.
    pub fn bindings(
        &self,
        dir: &Path,
        watch: &impl Fn(&Path) -> Result<(), String>,
    ) -> Result<Vars, String> {
        if self.env_files.is_empty() && !self.dev {
            return Ok(Vars::default());
        }
        let cwd = std::env::current_dir().unwrap_or_default();
        let log = |path: &Path| {
            let path = path.strip_prefix(&cwd).unwrap_or(path);
            eprintln!("Using secrets defined in {}", path.display());
        };
        if self.env_files.is_empty() {
            let path = dir.join(".dev.vars");
            match fs::read(&path) {
                Ok(bytes) => {
                    watch(&path)?;
                    log(&path);
                    return Ok(parse(&String::from_utf8_lossy(&bytes)));
                }
                Err(e) if e.kind() == io::ErrorKind::NotFound => {}
                Err(e) => return Err(format!("{}: {e}", path.display())),
            }
        }
        let (parsed, read) = read_all(&named(self.env_files, dir));
        for path in &read {
            watch(path)?;
            log(path);
        }
        Ok(expand(&parsed, Vars::default()))
    }
}

#[cfg(test)]
#[path = "dotenv-test.rs"]
mod tests;
