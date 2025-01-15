# VirtioFsSvc depends on WinFsp being installed.
cmd /c msiexec /qb /i E:\deps\winfsp.msi

# VirtioFsSvc should already have been installed by virtio-win-guest-tools.exe.
Set-Service -Name VirtioFsSvc -StartupType Automatic
