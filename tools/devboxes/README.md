# Devboxes

This directory contains tools to create virtual machine images to help you test workerd on different machines/OSes.

Right now there is only a Windows devbox.

## Pre-requisites

This directory has been tested with:

- QEMU 6.2.0
- libvirt 8.0.0
- aria2c 1.36.0
- Packer 1.11.2

### Ubuntu 22.04

On Ubuntu 22.04, [install Packer manually](https://developer.hashicorp.com/packer/install). The version in APT is too old.

The rest you can install from APT:

```sh
sudo apt-get install qemu-system-x86 libvirt-daemon-system aria2
```

You'll need to add yourself to the `kvm` and `libvirt` groups in order to use QEMU with KVM acceleration, and to communicate with libvirtd.

```sh
sudo adduser $(whoami) kvm
sudo adduser $(whoami) libvirt
# Remember to log out and in again for the groups to take effect!
```
