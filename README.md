hid-rival
=========

About
-----

hid-rival is a kernel module written for the Steelseries Rival family of mice
that exposes LED controls via the standard Linux sysfs LED interface.

Use at your own risk.

Support
-------

| Model | Supported |
|-|-|
| Rival 110 | :heavy_check_mark: |

Build
-----

Build the module (kernel headers are required):

    make
Then install it (requires root privileges, i.e. `sudo`):

    make install

DKMS support
------------

If you have DKMS installed, you can install hid-rival in such a way that it
survives kernel upgrades. It is recommended to remove older versions of hid-rival by running `dkms remove -m hid-rival -v OLDVERSION --all` as root. To install the new version, simply run:

    # make -f Makefile.dkms

To uninstall it, run:

    # make -f Makefile.dkms uninstall
