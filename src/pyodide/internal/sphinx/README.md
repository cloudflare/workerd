## Python SDK docs markdown generator

This directory contains config files for Sphinx, which can generate docs based on the exported methods in asgi.py and workers.py. These files are a part of the Python SDK.

### Usage


```
python -m venv .venv
source .venv/bin/activate
pip install -r requirements-doc.txt
make markdown
```

You'll then find the markdown in _build/markdown/docs/workers.md.

Make a PR in the cloudflare-docs repo to update the appropriate files.
