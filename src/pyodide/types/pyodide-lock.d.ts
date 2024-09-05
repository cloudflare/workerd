interface PackageDeclaration {
  depends: string[];
  file_name: string;
  imports: string[];
  install_dir: 'site' | 'stdlib';
  name: string;
  package_type: string;
  sha256: string;
  shared_library: boolean;
  unvendored_tests: boolean;
  version: string;
}

interface PackageLock {
  packages: {
    [id: string]: PackageDeclaration;
  };
}
