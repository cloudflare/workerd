winget install -e --id MSYS2.MSYS2 --version 20241116 --accept-source-agreements --accept-package-agreements

$msysPath = "C:\msys64\usr\bin"

$systemEnvPath = [System.Environment]::GetEnvironmentVariable('PATH', [System.EnvironmentVariableTarget]::Machine)
$systemEnvPath = "$msysPath;$systemEnvPath"
[System.Environment]::SetEnvironmentVariable('PATH', $systemEnvPath, [System.EnvironmentVariableTarget]::Machine)

Write-Output "Added $msysPath to the system PATH."

$env:PATH = "$msysPath;$env:PATH"

# MSYS has its own trust store, separate from Windows.
# https://www.msys2.org/docs/faq/
if ($env:CUSTOM_ROOT_CA.length -gt 0) {
  Write-Output "Adding custom root CA to MSYS /etc/pki/ca-trust/source/anchors/"
  Copy-Item "E:\$env:CUSTOM_ROOT_CA" "C:\msys64\etc\pki\ca-trust\source\anchors\"
  C:\msys64\usr\bin\bash.exe -c /usr/bin/update-ca-trust
}
