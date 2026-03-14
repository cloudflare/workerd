# Borrowed from https://github.com/asheroto/winget-install/blob/master/winget-install.ps1
function Install-NuGetIfRequired {
    <#
    .SYNOPSIS
    Checks if the NuGet PackageProvider is installed and installs it if required.

    .DESCRIPTION
    This function checks whether the NuGet PackageProvider is already installed on the system. If it is not found and the current PowerShell version is less than 7, it attempts to install the NuGet provider using Install-PackageProvider.
    For PowerShell 7 or greater, it assumes NuGet is available by default and advises reinstallation if NuGet is missing.

    .PARAMETER Debug
    Enables debug output for additional details during installation.

    .EXAMPLE
    Install-NuGetIfRequired
    # Checks for the NuGet provider and installs it if necessary.

    .NOTES
    This function only attempts to install NuGet if the PowerShell version is less than 7.
    For PowerShell 7 or greater, NuGet is typically included by default and does not require installation.
    #>

    # Check if NuGet PackageProvider is already installed, skip package provider installation if found
    if (-not (Get-PackageProvider -Name NuGet -ListAvailable -ErrorAction SilentlyContinue)) {
        Write-Debug "NuGet PackageProvider not found."

        # Check if running in PowerShell version less than 7
        if ($PSVersionTable.PSVersion.Major -lt 7) {
            # Install NuGet PackageProvider if running PowerShell version less than 7
            # PowerShell 7 has limited support for installing package providers, but NuGet is available by default in PowerShell 7 so installation is not required

            Write-Debug "Installing NuGet PackageProvider..."

            if ($Debug) {
                try { Install-PackageProvider -Name "NuGet" -Force -ForceBootstrap -ErrorAction SilentlyContinue } catch { }
            } else {
                try { Install-PackageProvider -Name "NuGet" -Force -ForceBootstrap -ErrorAction SilentlyContinue | Out-Null } catch {}
            }
        } else {
            # NuGet should be included by default in PowerShell 7, so if it's not detected, advise reinstallation
            Write-Warning "NuGet is not detected in PowerShell 7. Consider reinstalling PowerShell 7, as NuGet should be included by default."
        }
    } else {
        # NuGet PackageProvider is already installed
        Write-Debug "NuGet PackageProvider is already installed. Skipping installation."
    }
}

Write-Debug "Checking if NuGet PackageProvider is already installed..."
Install-NuGetIfRequired

PowerShellGet\Install-Module posh-git -Scope CurrentUser -Force
Add-PoshGitToProfile -AllUsers -AllHosts

# $profileContent = "`n$GitPromptSettings.DefaultPromptAbbreviateHomeDirectory = $true"
# Add-Content -LiteralPath $profile.AllUsersAllHosts -Value $profileContent -Encoding UTF8
