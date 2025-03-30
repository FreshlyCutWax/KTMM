Testing Environment Setup
=========================

This is a guide discussing how to setup the appropriate virtual machine for
testing.

Prerequisites
-------------

1. Install required packages.

        a. WSL Ubuntu::

                sudo apt install build-essential qemu-system virt-manager \
                bridge-utils virtiofsd zip unzip

2. Download the latest Debian 12 iso::

        wget "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-12.10.0-amd64-netinst.iso"


