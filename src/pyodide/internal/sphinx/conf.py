# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = "Python Workers API"
copyright = "2025, Cloudflare"
author = "Cloudflare"

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    "sphinx.ext.autodoc",
    "sphinx_markdown_builder",
    "sphinx.ext.autosummary",
    "myst_parser",
]
autodoc_mock_imports = ["js", "pyodide.http", "pyodide.ffi", "fastapi", "pyodide"]


templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]


# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = "alabaster"
html_static_path = ["_static"]

# Add the parent dir to the search path, so that the .py files can be accessed.
import sys
from pathlib import Path

sys.path.insert(0, str(Path.resolve(Path(".."))))
