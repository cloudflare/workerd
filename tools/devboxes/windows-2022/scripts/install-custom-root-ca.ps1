if ($env:CUSTOM_ROOT_CA.length -gt 0) {
  Import-Certificate -FilePath "E:\$env:CUSTOM_ROOT_CA" -CertStoreLocation Cert:\LocalMachine\Root
}
