load(":defs.bzl", _crate_repositories = "crate_repositories")

def _impl(_):
    _crate_repositories()

crate_repositories = module_extension(implementation = _impl)
