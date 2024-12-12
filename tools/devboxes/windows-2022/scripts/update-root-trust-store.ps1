# Windows only ships with a handful of trusted root certificates, then lazily-loads the rest from
# Windows Update as needed.
#
# This script downloads all the latest trusted root certificates from Windows Update proactively, so
# that tools which don't know how to trigger Windows' lazy-loading behavior (like Bazel) can use the
# Windows trust store right away.
#
# Sources:
# - https://woshub.com/updating-trusted-root-certificates-in-windows-10/#h2_3
# - https://stackoverflow.com/a/72969049

# Download certificates from Windows Update.
certutil.exe -generateSSTFromWU $env:TEMP\roots.sst

# Import the certificates into the root trust store.
$sstFile = (Get-ChildItem -Path $env:TEMP\roots.sst)
$sstFile | Import-Certificate -CertStoreLocation Cert:\LocalMachine\Root

Remove-Item -Path $env:TEMP\roots.sst
