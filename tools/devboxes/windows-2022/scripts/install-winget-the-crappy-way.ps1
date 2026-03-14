# This script appears to be the only way to install Winget in such a way that Packer can run it over
# WinRM. Original source: https://github.com/microsoft/winget-cli/issues/256#issuecomment-1416929101

Write-Output "Extracting WinGet and dependencies..."

New-Item -Path "$ENV:USERPROFILE" -Name "build" -ItemType "directory" -Force | Out-Null

Expand-Archive -Path "E:/deps/Microsoft.VCLibs.x64.14.Desktop.zip" -DestinationPath "$ENV:USERPROFILE/build/WinGet" -Force

Expand-Archive -Path "E:/deps/Microsoft.UI.Xaml.zip" -DestinationPath "$ENV:USERPROFILE/build/Microsoft.UI.Xaml"
Rename-Item -Path "$ENV:USERPROFILE/build/Microsoft.UI.Xaml/tools/AppX/x64/Release/Microsoft.UI.Xaml.2.7.appx" -NewName "Microsoft.UI.Xaml.2.7.zip"
Expand-Archive -Path "$ENV:USERPROFILE/build/Microsoft.UI.Xaml/tools/AppX/x64/Release/Microsoft.UI.Xaml.2.7.zip" -DestinationPath "$ENV:USERPROFILE/build/WinGet" -Force

Expand-Archive -Path "E:/deps/Microsoft.DesktopAppInstaller.zip" -DestinationPath "$ENV:USERPROFILE/build/Microsoft.DesktopAppInstaller"
Rename-Item -Path "$ENV:USERPROFILE/build/Microsoft.DesktopAppInstaller/AppInstaller_x64.msix" -NewName "AppInstaller_x64.zip"
Expand-Archive -Path "$ENV:USERPROFILE/build/Microsoft.DesktopAppInstaller/AppInstaller_x64.zip" -DestinationPath "$ENV:USERPROFILE/build/WinGet" -Force

Move-Item -Path "$ENV:USERPROFILE/build/WinGet" -Destination "$ENV:PROGRAMFILES" -PassThru
cmd /c setx /M PATH "%PATH%;$ENV:PROGRAMFILES\WinGet"

# I originally tried to install Winget "the right way" ...
#
# The most robust way to install Winget seems to be use this script:
# https://github.com/asheroto/winget-install
#
# On Windows Server 2022, the above winget-install script boils down to the method described here:
# https://github.com/microsoft/winget-cli/issues/4390
#
# But there are a couple catches:
# - The above methods use the Microsoft Store to install Winget for all users. Winget is not
#   available for a user until the user has logged in interactively for the first time, at which
#   point an asynchronous registration process starts, and Winget.exe eventually appears in the
#   user's PATH. To start using it immediately, you can force registration with a PowerShell
#   cmdlet: Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe
#   See: https://learn.microsoft.com/en-us/windows/package-manager/winget/#install-winget
# - Installed this way, Winget is not usable over WinRM:
#   https://github.com/microsoft/winget-cli/issues/256
