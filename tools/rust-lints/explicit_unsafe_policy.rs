//! `explicit_unsafe_policy` requires a local allow, deny, or forbid attribute
//! naming the bare built-in `unsafe_code` lint on every active authored module.
//! Crate roots, inline modules, external files, and authored input to attribute
//! macros are module boundaries. Test-only modules and their descendants are exempt.
//! Inheritance, warn/expect, function-level policies, and related unsafe lints do not
//! count. Multi-lint attributes and active cfg_attr policies do count. Rust itself
//! enforces the declared unsafe-code policy.
//!
//! Pre-expansion checks retain authored attribute-macro input; post-expansion checks
//! observe external files after their inner attributes have been loaded. Both passes
//! share source identities to emit one diagnostic per module. Macro-synthesized modules
//! are excluded, while source modules parsed by include! retain their source spans.
//! Suggestions are help text only: neither allow nor deny is safe to insert blindly.
//! The fixture driver uses command-line forbid to prevent source suppression.

#![deny(unsafe_code)]

use std::collections::HashSet;
use std::path::Component;
use std::path::Path;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::Mutex;

use rustc_ast::Attribute;
use rustc_ast::Item;
use rustc_ast::ItemKind;
use rustc_ast::ModKind;
use rustc_errors::DiagDecorator;
use rustc_expand::config::StripUnconfigured;
use rustc_lint::EarlyContext;
use rustc_lint::EarlyLintPass;
use rustc_lint::LintContext;
use rustc_lint::LintStore;
use rustc_session::declare_lint;
use rustc_session::impl_lint_pass;
use rustc_span::FileName;
use rustc_span::Span;
use rustc_span::Symbol;
use rustc_span::sym;

declare_lint! {
    pub EXPLICIT_UNSAFE_POLICY,
    Warn,
    "each authored module must explicitly declare its unsafe-code policy"
}

struct AuthoredModules {
    seen: Mutex<HashSet<Span>>,
    test_modules: Mutex<HashSet<Span>>,
    test_inner_attrs: Mutex<Vec<Span>>,
    test_spans: Mutex<Vec<Span>>,
    missing: Mutex<Vec<(Span, String)>>,
    // None is used by isolated compiler fixtures, which have no generated files.
    sources: Option<HashSet<PathBuf>>,
    working_directory: PathBuf,
}

type Inventory = Arc<AuthoredModules>;

impl AuthoredModules {
    fn new() -> Self {
        let working_directory = std::env::current_dir().expect("compiler working directory");
        let sources = std::env::var_os("WORKERD_RUST_LINT_SOURCES").map(|manifest| {
            std::fs::read_to_string(manifest)
                .expect("read Bazel authored-source manifest")
                .lines()
                .map(|path| normalized_path(&working_directory.join(path)))
                .collect()
        });
        Self {
            seen: Mutex::default(),
            test_modules: Mutex::default(),
            test_inner_attrs: Mutex::default(),
            test_spans: Mutex::default(),
            missing: Mutex::default(),
            sources,
            working_directory,
        }
    }

    fn contains(&self, cx: &EarlyContext<'_>, span: Span) -> bool {
        let Some(sources) = &self.sources else {
            return true;
        };
        let file = cx.sess().source_map().lookup_source_file(span.lo());
        if let FileName::Real(name) = &file.name
            && let Some(path) = name.local_path()
        {
            sources.contains(&normalized_path(&self.working_directory.join(path)))
        } else {
            false
        }
    }
}

// Source-map paths can contain module-relative `..` components. Normalize
// lexically rather than resolving sandbox/source symlinks through the host FS.
fn normalized_path(path: &Path) -> PathBuf {
    let mut result = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                result.pop();
            }
            component => result.push(component.as_os_str()),
        }
    }
    result
}

pub fn register(store: &mut LintStore) {
    store.register_lints(&[EXPLICIT_UNSAFE_POLICY]);
    let inventory = Arc::new(AuthoredModules::new());
    let pre_inventory = inventory.clone();
    store.register_pre_expansion_lint_pass(Box::new(move || {
        Box::new(PreExpansion {
            inventory: pre_inventory.clone(),
            active: Vec::new(),
        })
    }));
    store.register_early_lint_pass(Box::new(move || {
        Box::new(PostExpansion {
            inventory: inventory.clone(),
            parents: Vec::new(),
            modules: Vec::new(),
        })
    }));
}

fn has_policy(attrs: &[Attribute]) -> bool {
    attrs.iter().any(|attr| {
        [sym::allow, sym::deny, sym::forbid]
            .iter()
            .any(|name| attr.has_name(*name))
            && attr.meta_item_list().is_some_and(|items| {
                items.iter().any(|item| {
                    item.meta_item().is_some_and(|meta| {
                        meta.path.segments.len() == 1
                            && meta.has_name(Symbol::intern("unsafe_code"))
                    })
                })
            })
    })
}

fn check(
    cx: &EarlyContext<'_>,
    identity: Span,
    span: Span,
    name: &str,
    attrs: &[Attribute],
    inventory: &Inventory,
) {
    if !inventory.contains(cx, span)
        || !inventory.seen.lock().unwrap().insert(identity)
        || has_policy(attrs)
    {
        return;
    }
    // Loaded files are traversed independently before expansion. Defer emission
    // until their test-only ancestry is available in the post-expansion module tree.
    inventory
        .missing
        .lock()
        .unwrap()
        .push((span, name.to_owned()));
}

struct PreExpansion {
    inventory: Inventory,
    active: Vec<Option<rustc_ast::AttrVec>>,
}

impl_lint_pass!(PreExpansion => [EXPLICIT_UNSAFE_POLICY]);

impl EarlyLintPass for PreExpansion {
    fn check_attributes(&mut self, cx: &EarlyContext<'_>, attrs: &[Attribute]) {
        // The attribute hook also brackets LoadedMod roots, which have no check_item
        // callback. Their inner cfg attributes can disable the entire loaded file.
        let configured = if self.active.last().is_none_or(Option::is_some) {
            configure_attrs(cx, attrs).filter(|attrs| {
                if is_test_only(attrs) {
                    self.inventory.test_inner_attrs.lock().unwrap().extend(
                        attrs
                            .iter()
                            .filter(|attr| {
                                attr.style == rustc_ast::AttrStyle::Inner
                                    && is_test_only(std::slice::from_ref(*attr))
                            })
                            .map(|attr| attr.span),
                    );
                    false
                } else {
                    true
                }
            })
        } else {
            None
        };
        self.active.push(configured);
    }

    fn check_attributes_post(&mut self, _cx: &EarlyContext<'_>, _attrs: &[Attribute]) {
        self.active.pop();
    }

    fn check_crate(&mut self, cx: &EarlyContext<'_>, krate: &rustc_ast::Crate) {
        if let Some(Some(attrs)) = self.active.last() {
            let span = krate.spans.inner_span.shrink_to_lo();
            check(cx, span, span, "crate", attrs, &self.inventory);
        }
    }

    fn check_item(&mut self, cx: &EarlyContext<'_>, item: &Item) {
        if let ItemKind::Mod(_, ident, _) = &item.kind
            && self.active.last().is_some_and(Option::is_none)
        {
            self.inventory
                .test_modules
                .lock()
                .unwrap()
                .insert(ident.span);
            if let ItemKind::Mod(_, _, ModKind::Loaded(_, _, spans)) = &item.kind {
                self.inventory
                    .test_spans
                    .lock()
                    .unwrap()
                    .extend([item.span, spans.inner_span]);
            }
        }
        if let Some(Some(attrs)) = self.active.last()
            && let ItemKind::Mod(_, ident, ModKind::Loaded(..)) = &item.kind
            && !item.span.from_expansion()
        {
            // Inline and attribute-macro input is available before expansion. Unloaded
            // external modules are deferred until their file's inner attributes are attached.
            check(
                cx,
                ident.span,
                ident.span,
                ident.name.as_str(),
                attrs,
                &self.inventory,
            );
        }
    }
}

// Use rustc's cfg evaluator without cloning a module's entire AST or reimplementing
// cfg syntax. Only the attributes of this inert item are configured.
fn configure_attrs(cx: &EarlyContext<'_>, attrs: &[Attribute]) -> Option<rustc_ast::AttrVec> {
    let span = rustc_span::DUMMY_SP;
    let shell = Item {
        attrs: attrs.iter().cloned().collect(),
        id: rustc_ast::DUMMY_NODE_ID,
        span,
        vis: rustc_ast::Visibility {
            kind: rustc_ast::VisibilityKind::Inherited,
            span,
        },
        kind: ItemKind::ExternCrate(None, rustc_span::Ident::dummy()),
        tokens: None,
    };
    StripUnconfigured {
        sess: cx.sess(),
        features: None,
        config_tokens: false,
        lint_node_id: rustc_ast::CRATE_NODE_ID,
    }
    .configure(shell)
    .map(|item| item.attrs)
}

// Treat other cfg predicates as unknown: only a module that cannot exist without
// `test` is exempt. In particular, cfg(any(test, unix)) still contains production code.
fn without_test(meta: &rustc_ast::MetaItem) -> Option<bool> {
    if meta.has_name(sym::test) && matches!(meta.kind, rustc_ast::MetaItemKind::Word) {
        return Some(false);
    }
    let rustc_ast::MetaItemKind::List(items) = &meta.kind else {
        return None;
    };
    let values: Vec<_> = items
        .iter()
        .map(|item| item.meta_item().and_then(without_test))
        .collect();
    if meta.has_name(sym::all) {
        if values.contains(&Some(false)) {
            Some(false)
        } else if values.iter().all(|value| *value == Some(true)) {
            Some(true)
        } else {
            None
        }
    } else if meta.has_name(sym::any) {
        if values.contains(&Some(true)) {
            Some(true)
        } else if values.iter().all(|value| *value == Some(false)) {
            Some(false)
        } else {
            None
        }
    } else if meta.has_name(sym::not) && values.len() == 1 {
        values[0].map(|value| !value)
    } else {
        None
    }
}

fn is_test_only(attrs: &[Attribute]) -> bool {
    attrs.iter().any(|attr| {
        attr.has_name(sym::cfg)
            && attr.meta_item_list().is_some_and(|items| {
                items.len() == 1 && items[0].meta_item().and_then(without_test) == Some(false)
            })
    })
}

struct PostExpansion {
    inventory: Inventory,
    parents: Vec<Option<Span>>,
    // Identity, declaration span, body span, parent module identity.
    modules: Vec<(Span, Span, Span, Option<Span>)>,
}

impl_lint_pass!(PostExpansion => [EXPLICIT_UNSAFE_POLICY]);

impl EarlyLintPass for PostExpansion {
    fn check_crate(&mut self, _cx: &EarlyContext<'_>, krate: &rustc_ast::Crate) {
        let body = krate.spans.inner_span;
        let identity = body.shrink_to_lo();
        self.parents.push(Some(identity));
        self.modules.push((identity, body, body, None));
    }

    fn check_crate_post(&mut self, cx: &EarlyContext<'_>, _krate: &rustc_ast::Crate) {
        let mut excluded = self.inventory.test_spans.lock().unwrap();
        let mut test_modules = self.inventory.test_modules.lock().unwrap();
        // Inner cfg attributes on freshly loaded file roots are not attached to
        // an enclosing item in the pre-expansion traversal. Find their innermost
        // source module only after the complete module tree is available.
        for attr in self.inventory.test_inner_attrs.lock().unwrap().iter() {
            if excluded.iter().any(|span| span.contains(*attr)) {
                continue;
            }
            if let Some((identity, _, _, _)) = self
                .modules
                .iter()
                .filter(|(_, _, body, _)| body.contains(*attr))
                .min_by_key(|(_, _, body, _)| body.hi().0 - body.lo().0)
            {
                test_modules.insert(*identity);
            }
        }
        // The compiler visits parents before children, including out-of-line
        // modules whose bodies live in different source files.
        for (identity, declaration, body, parent) in &self.modules {
            if test_modules.contains(identity)
                || parent.is_some_and(|parent| test_modules.contains(&parent))
            {
                test_modules.insert(*identity);
                excluded.extend([*declaration, *body]);
            }
        }
        for (span, name) in self.inventory.missing.lock().unwrap().iter() {
            if excluded.iter().any(|excluded| excluded.contains(*span)) {
                continue;
            }
            cx.emit_span_lint(EXPLICIT_UNSAFE_POLICY, *span, DiagDecorator(|diag| {
                diag.primary_message(format!(
                    "module `{name}` must explicitly declare an unsafe-code policy"
                ));
                diag.help("add #![deny(unsafe_code)], #![forbid(unsafe_code)], or explicitly allow unsafe code in this module");
            }));
        }
        self.parents.pop();
    }

    fn check_item(&mut self, cx: &EarlyContext<'_>, item: &Item) {
        let parent = self.parents.last().copied().flatten();
        if let ItemKind::Mod(_, ident, ModKind::Loaded(_, inline, spans)) = &item.kind {
            self.parents.push(Some(ident.span));
            self.modules
                .push((ident.span, item.span, spans.inner_span, parent));
            if item.span.from_expansion() {
                return;
            }
            // External modules are diagnosed in the policy's source file, not at a
            // parent mod declaration whose policy may already be correct.
            let span = match inline {
                rustc_ast::Inline::No { .. } => spans.inner_span.shrink_to_lo(),
                rustc_ast::Inline::Yes => ident.span,
            };
            check(
                cx,
                ident.span,
                span,
                ident.name.as_str(),
                &item.attrs,
                &self.inventory,
            );
        } else {
            self.parents.push(parent);
        }
    }

    fn check_item_post(&mut self, _cx: &EarlyContext<'_>, _item: &Item) {
        self.parents.pop();
    }
}
