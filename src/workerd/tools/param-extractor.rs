use std::ffi::OsStr;
use std::fs::File;
use std::io::BufRead;
use std::io::BufReader;
use std::io::BufWriter;
use std::io::Write;
use std::path::Path;

use anyhow::Result;
use flate2::read::GzDecoder;
use serde::Deserialize;
use serde::Serialize;

/// Contains the declarations we care about
#[derive(Deserialize, PartialEq, Debug)]
enum Clang {
    NamespaceDecl { name: Option<String> },

    // Function-like -- direct parents of parameters
    FunctionDecl { name: Option<String> },
    CXXMethodDecl { name: Option<String> },
    CXXRecordDecl { name: Option<String> },
    CXXConstructorDecl,

    // Parameter names
    ParmVarDecl { name: Option<String> },

    // Everything else
    Other { name: Option<String> },
}

impl Clang {
    fn is_function_like(&self) -> bool {
        matches!(
            *self,
            Self::FunctionDecl { .. } | Self::CXXMethodDecl { .. } | Self::CXXConstructorDecl
        )
    }

    fn name(&self) -> Option<&str> {
        match self {
            Self::NamespaceDecl { name }
            | Self::FunctionDecl { name }
            | Self::CXXMethodDecl { name }
            | Self::CXXRecordDecl { name }
            | Self::ParmVarDecl { name }
            | Self::Other { name } => name.as_ref().map(AsRef::as_ref),
            Self::CXXConstructorDecl => Some("constructor"),
        }
    }
}

type ClangNode = clang_ast::Node<Clang>;

fn main() -> Result<()> {
    let mut args = pico_args::Arguments::from_env();

    let clang_ast = args.value_from_os_str("--input", |path_str| {
        let path = Path::new(path_str);
        let file = File::open(path)?;
        let serde = {
            let reader: &mut dyn BufRead = {
                if Some("gz") == path.extension().and_then(OsStr::to_str) {
                    &mut BufReader::new(GzDecoder::new(file))
                } else {
                    &mut BufReader::new(file)
                }
            };

            let mut deserializer = serde_json::Deserializer::from_reader(reader);
            // Note: serde_json doesn't support custom recursion limits, only disabling.
            // We disable the limit to handle deeply nested AST structures (default 128 is
            // insufficient for the clang AST dump, which can be deeply nested).
            deserializer.disable_recursion_limit();
            ClangNode::deserialize(&mut deserializer)
        };
        serde.map_err(anyhow::Error::from)
    })?;

    let value = get_parameter_names(clang_ast);

    let mut writer =
        args.value_from_os_str("--output", |path| File::create(path).map(BufWriter::new))?;

    serde_json::to_writer(&mut writer, &value)?;
    writer.flush()?;

    Ok(())
}

fn get_parameter_names(clang_ast: ClangNode) -> Vec<Parameter> {
    let workerd_namespace = Clang::NamespaceDecl {
        name: Some("workerd".to_owned()),
    };

    clang_ast
        .inner
        .into_iter()
        .filter(|node| node.kind == workerd_namespace)
        .flat_map(|node| traverse_disambiguous(node, &[]))
        .collect()
}

#[derive(Serialize, Debug)]
struct Parameter {
    fully_qualified_parent_name: Vec<String>,
    function_like_name: String,
    index: usize,
    name: String,
}

fn traverse_disambiguous(
    disambiguous: ClangNode,
    fully_qualified_parent_name: &[String],
) -> Vec<Parameter> {
    let disambiguous_name = disambiguous
        .kind
        .name()
        .map(ToOwned::to_owned)
        .unwrap_or_default();

    disambiguous
        .inner
        .into_iter()
        .flat_map(|node| {
            let mut qualified: Vec<_> = fully_qualified_parent_name.to_vec();
            qualified.push(disambiguous_name.clone());
            if node.kind.is_function_like() {
                traverse_function_like(node, &qualified)
            } else {
                traverse_disambiguous(node, &qualified)
            }
        })
        .collect()
}

fn traverse_function_like(
    node: ClangNode,
    fully_qualified_parent_name: &[String],
) -> Vec<Parameter> {
    let function_like_name = node.kind.name().expect("missing name").to_owned();

    node.inner
        .into_iter()
        .filter_map(|child| {
            if let Clang::ParmVarDecl { name: Some(name) } = child.kind {
                Some(name)
            } else {
                None
            }
        })
        .enumerate()
        .map(|(i, param_name)| Parameter {
            fully_qualified_parent_name: fully_qualified_parent_name.to_vec(),
            function_like_name: function_like_name.clone(),
            index: i,
            name: param_name,
        })
        .collect()
}
