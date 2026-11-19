// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The checks Worker Loader applies to a dynamic Worker's code, in plain Rust.
//!
//! They are which kind of module a name stands for, the limits on code and `env` size, and the
//! flags that Python Workers need. C++ `compileCompatibilityFlags()` compiles the flags. Each
//! error's message is word for word the one `api/worker-loader.c++` throws.

#![expect(
    clippy::case_sensitive_file_extension_comparisons,
    reason = "module names are matched case-sensitively, as `api/worker-loader.c++` matches them"
)]

/// The largest total size, in bytes, of a dynamic Worker's module bodies: the documented paid
/// Worker uncompressed size limit.
pub const MAX_CODE_SIZE: usize = 64 * 1024 * 1024;

/// The largest estimated size, in bytes, of a dynamic Worker's serialized `env`: roughly the
/// paid Worker analog of 128 environment variables at 5 KB each.
pub const MAX_ENV_SIZE: usize = 1024 * 1024;

/// The kind of a module, as C++ `Worker::Script::ModuleContent` distinguishes them.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModuleKind {
    Es,
    CommonJs,
    Text,
    Data,
    Json,
    Python,
    Wasm,
}

/// What JavaScript sees a failed check as.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorKind {
    Error,
    TypeError,
}

/// A failed check.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ValidationError {
    pub kind: ErrorKind,
    pub message: String,
}

impl ValidationError {
    fn error(message: String) -> Self {
        Self {
            kind: ErrorKind::Error,
            message,
        }
    }

    fn type_error(message: String) -> Self {
        Self {
            kind: ErrorKind::TypeError,
            message,
        }
    }
}

pub type Result<T> = std::result::Result<T, ValidationError>;

/// Whether `main_module` names a Python module, which makes the Worker a Python Worker.
#[must_use]
pub fn is_python_main(main_module: &str) -> bool {
    main_module.ends_with(".py")
}

/// Fails unless the Worker has at least one module.
///
/// # Errors
/// A `TypeError` for no modules.
pub fn require_modules(count: usize) -> Result<()> {
    if count == 0 {
        return Err(ValidationError::type_error(
            "Dynamic Worker code must contain at least one module.".to_owned(),
        ));
    }
    Ok(())
}

/// The kind of a module given as a bare string, by its name: `.py` is Python, `.js` an ES module,
/// and, in a Python Worker, anything under `python_modules/` text.
///
/// # Errors
/// A `TypeError` for any other name, which suggests bundling for TypeScript and JSX names.
pub fn classify_string_module(name: &str, main_module: &str) -> Result<ModuleKind> {
    if name.ends_with(".py") {
        return Ok(ModuleKind::Python);
    }
    if name.ends_with(".js") {
        return Ok(ModuleKind::Es);
    }
    // Python packages bundled in Workers can have non-code files (METADATA, RECORD, etc), so file
    // extensions aren't limited for Python Workers.
    if is_python_main(main_module) && name.starts_with("python_modules/") {
        return Ok(ModuleKind::Text);
    }
    let mut message = format!(
        "Module name must end with '.js' or '.py' (or the content must be an object indicating \
         the type explicitly). Got: {name}"
    );
    if name.ends_with(".ts") || name.ends_with(".tsx") || name.ends_with(".jsx") {
        message.push_str(
            ". If you're trying to load TypeScript, bundle it first with \
             '@cloudflare/worker-bundler' and pass the generated JavaScript modules.",
        );
    }
    Err(ValidationError::type_error(message))
}

/// Fails unless a module object set exactly one of its fields; `count` is how many it set.
///
/// # Errors
/// A `TypeError` naming the module and the count.
pub fn require_exactly_one_field(name: &str, count: usize) -> Result<()> {
    if count != 1 {
        return Err(ValidationError::type_error(format!(
            "Each module must contain exactly one of 'js', 'cjs', 'text', 'data', 'json', 'py', \
             or 'wasm'. Module '{name}' contained {count} properties."
        )));
    }
    Ok(())
}

/// Checks the modules once each has its kind and size.
///
/// A JavaScript Worker may hold no Python module, and the bodies may total at most
/// [`MAX_CODE_SIZE`] bytes. Python Workers may hold any module, since Python packages can contain
/// arbitrary `.js` files.
///
/// # Errors
/// A `TypeError` for the first Python module of a JavaScript Worker, otherwise an `Error` if the
/// code is too large.
pub fn check_modules<'a>(
    main_module: &str,
    modules: impl IntoIterator<Item = (&'a str, ModuleKind, usize)>,
) -> Result<()> {
    let is_python = is_python_main(main_module);
    let mut total: usize = 0;
    for (name, kind, size) in modules {
        if !is_python && kind == ModuleKind::Python {
            return Err(ValidationError::type_error(format!(
                "Module \"{name}\" is a Python module, but the main module isn't a Python module."
            )));
        }
        total = total.saturating_add(size);
    }
    if total > MAX_CODE_SIZE {
        return Err(ValidationError::error(format!(
            "Dynamic Worker code size ({total} bytes) exceeds the maximum allowed size of \
             {MAX_CODE_SIZE} bytes."
        )));
    }
    Ok(())
}

/// The compatibility flags of a Worker, with those a Python Worker needs.
///
/// A Python Worker needs `python_workers` and, unless the caller says otherwise,
/// `disable_python_external_sdk`, since dynamic Workers can't include the SDK yet. Any other
/// Worker's flags are returned as they are.
#[must_use]
pub fn python_flags(main_module: &str, flags: Option<Vec<String>>) -> Option<Vec<String>> {
    if !is_python_main(main_module) {
        return flags;
    }
    let given = flags.as_deref().unwrap_or_default();
    let has = |flag: &str| given.iter().any(|f| f == flag);
    let has_python_workers = has("python_workers");
    // TODO: The external SDK is disabled because dynamic Workers have no way to include it.
    // Once RM-28738 is implemented this may go away.
    let add_sdk_disable = !has("enable_python_external_sdk") && !has("disable_python_external_sdk");
    if has_python_workers && !add_sdk_disable {
        return flags;
    }
    let mut flags = flags.unwrap_or_default();
    if !has_python_workers {
        flags.push("python_workers".to_owned());
    }
    if add_sdk_disable {
        flags.push("disable_python_external_sdk".to_owned());
    }
    Some(flags)
}

/// Fails if the code asks for experimental features but the calling Worker doesn't have the
/// `experimental` compatibility flag.
///
/// # Errors
/// An `Error`.
pub fn check_allow_experimental(allow_experimental: bool, caller_experimental: bool) -> Result<()> {
    if allow_experimental && !caller_experimental {
        return Err(ValidationError::error(
            "'allowExperimental' is only allowed when the calling worker has the 'experimental' \
             compat flag set."
                .to_owned(),
        ));
    }
    Ok(())
}

/// Fails if the code names streaming tail Workers without allowing experimental features.
///
/// # Errors
/// An `Error`.
pub fn check_streaming_tails(allow_experimental: bool) -> Result<()> {
    if !allow_experimental {
        return Err(ValidationError::error(
            "Streaming tail workers are experimental. You must pass the option \
             'allowExperimental: true' to the worker loader to use them"
                .to_owned(),
        ));
    }
    Ok(())
}

/// Fails if the estimated size of the serialized `env` is over [`MAX_ENV_SIZE`].
///
/// # Errors
/// An `Error`.
pub fn check_env_size(estimate: usize) -> Result<()> {
    if estimate > MAX_ENV_SIZE {
        return Err(ValidationError::error(format!(
            "Dynamic Worker env size ({estimate} bytes) exceeds the maximum allowed size of \
             {MAX_ENV_SIZE} bytes."
        )));
    }
    Ok(())
}

/// The entrypoint a name asks for: `None`, for the default one, if it is absent, `null` or
/// `"default"`.
#[must_use]
pub fn entrypoint_name(name: Option<String>) -> Option<String> {
    name.filter(|name| name != "default")
}

#[cfg(test)]
#[path = "validate-test.rs"]
mod tests;
