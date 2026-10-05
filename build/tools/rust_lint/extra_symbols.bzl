"""Declared OUT_DIR for Dylint builds that do not load Clippy utilities."""

def _extra_symbols_impl(ctx):
    out_dir = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run_shell(
        outputs = [out_dir],
        arguments = [out_dir.path],
        command = "mkdir -p \"$1\"; printf '%s\\n' 'const EXTRA_SYMBOLS: &[&str] = &[];' > \"$1/extra_symbols.rs\"",
        mnemonic = "DylintExtraSymbols",
    )
    return [DefaultInfo(files = depset([out_dir]))]

dylint_extra_symbols = rule(implementation = _extra_symbols_impl)
