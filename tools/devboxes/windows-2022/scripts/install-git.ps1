winget install -e --id Git.Git --version 2.47.1 --accept-source-agreements --accept-package-agreements

$gitPath = "C:\Program Files\Git\bin"

$systemEnvPath = [System.Environment]::GetEnvironmentVariable('PATH', [System.EnvironmentVariableTarget]::Machine)
$systemEnvPath = "$gitPath;$systemEnvPath"
[System.Environment]::SetEnvironmentVariable('PATH', $systemEnvPath, [System.EnvironmentVariableTarget]::Machine)

Write-Output "Added $gitPath to the system PATH."

$env:PATH = "$gitPath;$env:PATH"

# Configure git to use the Windows Trust Store, to pick up any custom root CA we installed.
Write-Output "Configuring Git to use the Windows Trust Store (schannel)"
git config --global http.sslBackend schannel
