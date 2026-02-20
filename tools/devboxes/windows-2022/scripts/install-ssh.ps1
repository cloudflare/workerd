# https://github.com/PowerShell/Win32-OpenSSH/wiki/Install-Win32-OpenSSH
winget install -e --id Microsoft.OpenSSH.Beta --version 9.5.0.0 --accept-source-agreements --accept-package-agreements

New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server (sshd)' -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
