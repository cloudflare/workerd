# Windows Server 2022 development box

This directory contains installation and provisioning Packer templates for Windows Server 2022 Desktop.

Note: Up until recently, Microsoft used to publish "dev VMs". As of Octoboer 23, 2024, they are unavailable ["due to ongoing technical issues"](https://developer.microsoft.com/en-us/windows/downloads/virtual-machines/). If they ever come back, we could use them instead of this manually-built VM image.

Note: [Winget does not currently work on Windows Server 2022 Core](https://github.com/microsoft/winget-cli/issues/4319), which is why we use the Desktop Experience instead.

This directory is split into two separate Packer templates:
- `windows-2022/base` (installation, based on https://github.com/StefanScherer/packer-windows)
- `windows-2022` (provisioning, this directory)

The reason for this split into two Packer builds is to make it easier to hack on the provisioning scripts without having to wait for a full re-installation. The `windows-2022` template is configured with a copy-on-write image using the `windows-2022/base` template's read-only image as a backing file, so the `windows-2022` image can be relatively quickly recreated.

Steps to build and use this devbox:
1. Install pre-requisites
1. Fetch dependencies
1. Configure custom root CA (if applicable)
1. Run `packer build`
1. Import the resulting .qcow2 file into a VM with `virt-manager`
1. Make an SSH config on host OS
1. Configure Git on guest OS
1. (Optional) Set up a shared drive

## Install pre-requisites

As described in the parent directory's [README.md](../README.md).

## Fetch dependencies

There's a script in this directory, `fetch-deps.sh`, which uses `aria2c` to download some binary dependencies for the Packer builds. You can run it from any directory, and the dependencies will end up in this directory (and the `base/` subdirectory).

```sh
./fetch-deps.sh
```

The script also creates a couple of `deps.auto.pkrvars.hcl` files, which tell our Packer templates the absolute paths to the dependency directories. Without these files, our Packer builds will produce errors nudging you to run `fetch-deps.sh`.

## Configure custom root CA (if applicable)

If you need to use a custom root CA (e.g., you use Cloudflare One / Warp), create a file in this directory like so:

```
# custom-root-ca.auto.pkrvars.hcl
custom_root_ca = "/path/to/my/ca.pem"
```

If you do not need to use a custom root CA, you will still need to configure this variable. Set it to the empty string to indicate no custom root CA is required.

```
# custom-root-ca.auto.pkrvars.hcl
custom_root_ca = ""  # intentionally empty
```

The reason this variable is required is to make it harder to accidentally forget to embed a custom root CA if you do need one, which leads to lots of follow-on troubleshooting.

## Run `packer build`

Run `packer build` against first this directory's `base/` subdirectory, then this directory itself.

```sh
packer build ./base
packer build .
```

This will produce two output directories in your current working directory, named `output-windows-2022-base/` and `output-windows-2022/`. Each of these directories has a QCOW2 image inside it, named `windows-2022-base.qcow2` and `windows-2022.qcow2`. The first image is a read-only snapshot of a fresh installation of Windows. The second image is a copy-on-write overlay using the first as a backing file, and contains Windows plus various utilities provisioned: your custom root CA, virtiofs, Winget, SSH, Git, MSYS, etc.

Changing the first, base image (`windows-2022-base.qcow2`) in any way will corrupt the overlay (`windows-2022.qcow2`). If you would like a standalone QCOW2 image, you can use `qemu-img` to convert it. This takes a while, which is why the Packer template doesn't do this for you.

## Import the resulting .qcow2 file into a VM with `virt-manager`

Run `virt-manager`, and create a new virtual machine, importing `output-windows-2022/windows-2022.qcow2`. Give the machine plenty of RAM and CPU cores (I use 16G RAM and 16 cores).

You can now develop directly inside of the Windows desktop via virt-manager's VM console, if you wish.

## Make an SSH config on host OS

Once your VM boots, take note of its IP address so you can SSH into it. You can find the VM's IP address in `virt-manager` by looking at the VM's info page and selecting its NIC after it has started up for the first time.

Add the following to your `~/.ssh/config`:

```
Host winbox
  User vagrant
  HostName <VM IP ADDRESS>
```

You should now be able to use VSCode's Remote-SSH extension running on your host OS to connect to the `winbox` SSH host, and develop remotely.

Note that if you ever delete and recreate the VM, it will likely be assigned a new IP address.

## Configure Git on guest OS

Remember to configure your contact information:

```sh
git config --global user.name "My Name"
git config --global user.email "my@example.com"
```

You'll also need some way of authenticating with GitHub. One easy way is to generate a (classic) GitHub Personal Access Token with access to `repos`. First, configure a simple credential store, like DPAPI, on the guest OS:

```sh
git config --global credential.credentialStore dpapi
```

Then create your access token in the GitHub developer settings page, and add it like so:

```sh
git credential approve
# Enter the following on stdin:
protocol=https
host=github.com
username=your_github_username
password=your_github_personal_access_token
# Press enter twice
```

Now you can push from your Windows guest OS to repos you have write access to.

## Set up a shared drive

To configure a shared drive for your VM, first shut the VM down if it is already running.

Next, go to the VM's info page in `virt-manager`, click on the "Memory" panel, check "Enable shared memory", and click "Apply".

Lastly, from the VM's info page, click "Add Hardware" -> "Filesystem" -> ensure "Driver is "virtiofs", and fill in the "Source path" to point to your desired host-side shared directory. Put whatever identifier you want in the "Target path" textbox, it doesn't matter.

The next time you start the VM, you should see your shared directory available as drive Z:\.

# Options

## Graphical output, headless mode

By default, the build displays the QEMU console, which shows the VM's graphical output. This allows you to observe the Windows installation process directly.

To inhibit this behavior, add `--var headless=true` to your `packer build` command.

# Troubleshooting

## Observability tools

Your two basic tools for gaining visibility onto what is going on in the Packer build is setting the `PACKER_LOG=1` environment variable, and viewing the VM's graphical output, via the QEMU console (`--var headless=false`). (It's also possible to connect via VNC, even in headless mode.)

## `qemu: Waiting for WinRM to become available...` hangs forever

Check the QEMU console to see what the VM's graphical output tells you.

## Packer fails with a qemu-img error "Backing file specified without backing format"

You are using QEMU version >= 6.1 and Packer QEMU Plugin < 1.1.0. Upgrade your plugin with `packer init`.

## QEMU fails with" /usr/bin/qemu-system-x86_64: symbol lookup error: /snap/core20/current/lib/x86_64-linux-gnu/libpthread.so.0: undefined symbol: __libc_pthread_init, version GLIBC_PRIVATE"

You are probably running Packer inside a VS Code terminal. Try running Packer in a regular terminal.

I don't know the exact cause of this failure, but it affects multiple programs, not just QEMU.

## Windows shows a BSOD during boot with stop code 0x0000007b / INACCESSIBLE_BOOT_DEVICE.

This can be caused if you change the size of the hard drive after installation. In particular, I encountered it when `windows-2022-base`'s Packer template used a different hard disk size than `windows-2022`'s template.

It may be possible to set up a post-login script which hacks around this issue, as described here: https://superuser.com/a/1439626

There can be many other causes of this BSOD as well, unfortunately.
