# Windows Server 2022 Desktop installation template
#
# This was originally bootstrapped by copying
# https://github.com/StefanScherer/packer-windows/blob/main/windows_2022.json. I then converted it
# to HCL, de-Vagranted it, and removed several scripts which weren't useful for our use case.

packer {
  required_plugins {
    qemu = {
      source  = "github.com/hashicorp/qemu"
      version = "~> 1.1.0"
    }
  }
}

variables {
  cpus = 2
  # Disk size must match the disk size in windows-2022.pkr.hcl, or else you will get a BSOD!
  disk_size = 61440
  headless  = false
  memory    = 8192
}

# The `installer_iso` variable contains a link to the current Windows Server 2022
# English (United States) ISO image.
#
# To update the link:
#
# 1. Visit https://www.microsoft.com/en-us/evalcenter/download-windows-server-2022.
#
# 2. Use `curl` to GET the URL for the English (United States) ISO. Expect to receive a
#    301 Moved Permanently response from the origin.
#
# 3. Copy the response's Location header value to this variable's default value below.
#
# 4. Run `packer build`. When the checksum fails, make note of the new checksum, and update the
#    `installer_iso_checksum` variable's default value below.
variable "installer_iso" {
  type    = string
  default = "https://software-static.download.prss.microsoft.com/sg/download/888969d5-f34g-4e03-ac9d-1f9786c66749/SERVER_EVAL_x64FRE_en-us.iso"
}

variable "installer_iso_checksum" {
  type    = string
  default = "sha256:3e4fa6d8507b554856fc9ca6079cc402df11a8b79344871669f0251535255325"
}

# The `virtio_win_iso` variable identifies the path to the VirtIO Windows drivers ISO. It is
# mandatory.
#
# To set this variable, run the `fetch-deps.sh` script next to the main `windows-2022.pkr.hcl`
# Packer template. The script will download the VirtIO ISO file into the right place, and create
# a deps.auto.pkrvars.hcl file next to _this_ Packer template which sets this variable.
variable "virtio_win_iso" {
  description = "Path to virtio-win.iso"
  type        = string
  default     = ""

  validation {
    condition     = length(var.virtio_win_iso) > 0
    error_message = "Run `fetch-deps.sh` to set this variable."
  }

  validation {
    condition     = fileexists(var.virtio_win_iso)
    error_message = "`virtio_win_iso` file does not exist. Run `fetch-deps.sh` to set this variable."
  }
}

# The `output_qcow2` variable is the name of the generated VM snapshot, ready for importing into
# virt-manager, or running directly with QEMU.
variable "output_qcow2" {
  type    = string
  default = "windows-2022-base.qcow2"
}

locals {
  output_directory = "output-windows-2022-base"
}

source "qemu" "windows-2022-base" {
  # Hardware
  accelerator = "kvm"
  headless    = var.headless
  cpus        = var.cpus
  memory      = var.memory
  disk_size   = var.disk_size

  # Input files
  floppy_files = [
    # The Autounattend.xml chooses whether to install 2022 Desktop or Core.
    "${path.root}/answer_files/2022/Autounattend.xml",
    "${path.root}/scripts/disable-screensaver.ps1",
    "${path.root}/scripts/disable-winrm.ps1",
    "${path.root}/scripts/enable-winrm.ps1",
    "${path.root}/scripts/microsoft-updates.bat",
    "${path.root}/scripts/win-updates.ps1"
  ]

  # Input image
  iso_url      = var.installer_iso
  iso_checksum = var.installer_iso_checksum

  # Output image
  output_directory = local.output_directory
  vm_name          = var.output_qcow2
  format           = "qcow2"

  # Since we need to attach multiple drives, we must override `qemuargs`. This ends up overriding
  # a lot of other disk-related configuration properties in the `qemu` data source.
  qemuargs = [
    ["-drive", "file=${local.output_directory}/${var.output_qcow2},if=virtio,cache=writeback,discard=ignore,format=qcow2,index=1"],
    ["-drive", "file=${var.installer_iso},media=cdrom,index=2"],
    ["-drive", "file=${var.virtio_win_iso},media=cdrom,index=3"]
  ]

  # Give a little time for QEMU's VNC server to start.
  boot_wait = "1s"

  communicator   = "winrm"
  winrm_password = "vagrant"
  winrm_username = "vagrant"
  # We have to wait for Windows to install itself before we can connect with WinRM. This typically
  # takes 10 to 30 minutes on my machine.
  winrm_timeout = "2h"

  shutdown_command = "shutdown /s /t 10 /f /d p:4:1 /c \"Packer Shutdown\""
}

build {
  sources = ["source.qemu.windows-2022-base"]

  provisioner "windows-shell" {
    scripts = ["${path.root}/scripts/enable-rdp.bat"]
  }

  provisioner "windows-shell" {
    scripts = [
      "${path.root}/scripts/set-winrm-automatic.bat",
      "${path.root}/scripts/uac-enable.bat",
      "${path.root}/scripts/dis-updates.bat",
    ]
  }

  # We make the resulting image read-only, with the idea that you should use it as a backing file
  # for a new copy-on-write image. This allows you to easily revert to a pristine snapshot of a
  # freshly-installed Windows at any time.
  post-processor "shell-local" {
    inline = [
      "chmod 0444 ${local.output_directory}/${var.output_qcow2}",
      # Run qemu-img to make backing file.
      # Print something out.
    ]
  }

  post-processor "checksum" {
    checksum_types = ["sha256"]
    output         = "${local.output_directory}/${var.output_qcow2}.{{.ChecksumType}}"
  }
}
