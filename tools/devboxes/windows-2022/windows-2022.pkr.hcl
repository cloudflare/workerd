# Windows Server 2022 Desktop provisioning template
#
# This template takes the output image of the `base/` template (relative to this directory), and
# configures a custom root CA and installs various tools useful for workerd development.

packer {
  required_plugins {
    qemu = {
      version = "~> 1.1.0"
      source  = "github.com/hashicorp/qemu"
    }
  }
}

variables {
  cpus = 2
  # Disk size must match the disk size in windows-2022-base.pkr.hcl, or else you will get a BSOD!
  disk_size = 61440
  headless  = false
  memory    = 8192
}

# The `custom_root_ca` variable allows you to add a custom trusted root CA to the Windows guest's
# trust store.
#
# If your organization uses a custom root CA to inspect traffic, such as can be done with
# [Cloudflare One](https://developers.cloudflare.com/cloudflare-one/connections/connect-devices/user-side-certificates/),
# you'll need to populate this variable with the path to the CA's PEM-encoded certificate.
#
# You must explicitly set this feature, because if you need a custom root CA, it's a pain if you
# accidentally build the image without one.
variable "custom_root_ca" {
  type = string
}

variable "deps_dir" {
  description = "Path to deps/ with winfsp.msi, etc. inside"
  type        = string
  default     = ""

  validation {
    condition     = length(var.deps_dir) > 0
    error_message = "Run `fetch-deps.sh` to set this variable."
  }
}

variable "input_qcow2" {
  type    = string
  default = "output-windows-2022-base/windows-2022-base.qcow2"
}

variable "output_qcow2" {
  type    = string
  default = "windows-2022.qcow2"
}

locals {
  # Read the SHA256 from the file produced by Packer's Checksum post-processor.
  input_qcow2_sha256 = split("\t", file("${path.cwd}/${var.input_qcow2}.sha256"))[0]
  output_directory   = "output-windows-2022"
}

source "qemu" "windows-2022" {
  # Hardware
  accelerator = "kvm"
  headless    = var.headless
  cpus        = var.cpus
  memory      = var.memory
  disk_size   = var.disk_size

  cd_files = flatten([
    # Optionally include a custom root CA. We do not check fileexists(), to guard against misspelled
    # filenames.
    length(var.custom_root_ca) > 0 ? [var.custom_root_ca] : [],
    var.deps_dir
  ])

  # Input image
  iso_url      = var.input_qcow2
  iso_checksum = local.input_qcow2_sha256

  # Output image
  output_directory = local.output_directory
  vm_name          = var.output_qcow2
  format           = "qcow2"

  # Use the input image as a backing file for the output image.
  disk_image       = true
  use_backing_file = true
  skip_resize_disk = true
  skip_compaction  = true
  disk_compression = false

  # Give a little time for QEMU's VNC server to start.
  boot_wait = "1s"

  communicator   = "winrm"
  winrm_username = "vagrant"
  winrm_password = "vagrant"
  # We expect the machine to boot reasonably quickly.
  winrm_timeout = "10m"

  shutdown_command = "shutdown /s /t 10 /f /d p:4:1 /c \"Packer Shutdown\""
}

build {
  sources = ["source.qemu.windows-2022"]

  provisioner "powershell" {
    environment_vars = ["CUSTOM_ROOT_CA=${basename(var.custom_root_ca)}"]
    scripts = [
      "${path.root}/scripts/install-custom-root-ca.ps1",
      "${path.root}/scripts/fix-virtiofs.ps1",
      "${path.root}/scripts/install-winget-the-crappy-way.ps1",
      "${path.root}/scripts/install-ssh.ps1",
      # Git and MSYS both come with Bash environments. Each install script prepends their /bin/ to
      # the existing PATH, so earlier scripts' environments will be found later in the PATH.
      "${path.root}/scripts/install-git.ps1",
      "${path.root}/scripts/install-msys.ps1"
      # TODO: Bazel custom root CA config
    ]
  }
}
