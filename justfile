prepare:
  cargo install gen-compile-commands

compile-commands:
  rm -f compile_commands.json | gen-compile-commands --root . --compile-flags compile_flags.txt --out compile_commands.json --src-dir ./src

clean:
  rm -f compile_commands.json
