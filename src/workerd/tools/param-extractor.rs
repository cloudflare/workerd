use std::{
    ffi::OsStr,
    fs::File,
    io::{BufReader, BufWriter, Write},
    path::Path,
};

use anyhow::Result;
use flate2::read::GzDecoder;
use serde::{Deserialize, Serialize};

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
            Clang::NamespaceDecl { name }
            | Clang::FunctionDecl { name }
            | Clang::CXXMethodDecl { name }
            | Clang::CXXRecordDecl { name }
            | Clang::ParmVarDecl { name }
            | Clang::Other { name } => name.as_ref().map(AsRef::as_ref),
            Clang::CXXConstructorDecl => Some("constructor"),
        }
    }
}

type ClangNode = clang_ast::Node<Clang>;

fn main() -> Result<()> {
    let mut args = pico_args::Arguments::from_env();

    let clang_ast = args.value_from_os_str("--input", |path_str| {
        let path = Path::new(path_str);
        let file = File::open(path)?;
        let serde = match path.extension().and_then(OsStr::to_str) {
            Some("gz") => serde_json::from_reader(BufReader::new(GzDecoder::new(file))),
            _ => serde_json::from_reader(BufReader::new(file))
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
        .flat_map(|node| traverse_disambiguous(node, vec![]))
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
    fully_qualified_parent_name: Vec<String>,
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
            if node.kind.is_function_like() {
                let mut qualified = fully_qualified_parent_name.clone();
                qualified.push(disambiguous_name.clone());
                traverse_function_like(node, qualified)
            } else {
                let mut qualified = fully_qualified_parent_name.clone();
                qualified.push(disambiguous_name.clone());
                traverse_disambiguous(node, qualified)
            }
        })
        .collect()
}

fn traverse_function_like(
    node: ClangNode,
    fully_qualified_parent_name: Vec<String>,
) -> Vec<Parameter> {
    let function_like_name = node.kind.name().unwrap().to_owned();

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
            fully_qualified_parent_name: fully_qualified_parent_name.clone(),
            function_like_name: function_like_name.clone(),
            index: i,
            name: param_name,
        })
        .collect()
}
