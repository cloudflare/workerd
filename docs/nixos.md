# Why is there a need for a fix?

NixOS can't run dynamically linked executables for generic Linux systems out of the box. This is due to its unique design, which lacks a global library path and doesn't adhere to the Filesystem Hierarchy Standard (FHS).

To continue working with workerd, simply add the following code to your NixOS configuration and rebuild the system.

```nix
  programs.nix-ld.enable = true;
  programs.nix-ld.libraries = with pkgs; [
    stdenv.cc.cc
    zlib
    fuse3
    icu
    nss
    openssl
    curl
    expat
  ];
```

**References**:
[official nixos documentation](https://nix.dev/guides/faq#how-to-run-non-nix-executables)
[nix-ld](https://github.com/Mic92/nix-ld)
[cloudflare/workerd/issue#1515](https://github.com/cloudflare/workerd/discussions/1515)

