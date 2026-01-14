import _cloudflare_compat_flags
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def test(self):
        assert _cloudflare_compat_flags.python_workflows
        assert not _cloudflare_compat_flags.python_no_global_handlers
