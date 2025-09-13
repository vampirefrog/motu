MOTU midi linux driver
==================================

| Device           | Ports | Status          |
|------------------|-------|-----------------|
| midi express 128 | 8+8   | Working         |
| micro lite       | 5+5   | Working         |
| MIDI Express XT  | 8+8   | MIDI ports only |
| micro express    | 5+5   | MIDI ports only |

There is currently no replacement for ClockWorks either.

Only tested on linux 5.x.

Currently in alpha stage, please submit pull requests or bug reports if you find any.

Build:
------

Make sure you have linux headers or linux source.

```bash
sudo apt-get install linux-headers-`uname -r`
```

Build and install.

```bash
make -C /lib/modules/`uname -r`/build M=$PWD
sudo make -C /lib/modules/`uname -r`/build M=$PWD modules_install
sudo depmod
sudo modprobe motu
```

DKMS
----

There is also a DKMS file for Arch linux.

A script has been added by darkQ to build a debian package with an adapted DKMS file.
```bash
./debpkg.sh
sudo apt install ./motu-*.deb
```


Protocol:
---------

The device doesn't use regular USB MIDI descriptors and packets, which is the reason why there was no linux driver.
I ran the windows driver in a VM and captured the USB packets on the host with Wireshark.

The protocol is reverse engineered:

Data is double encoded. One encoding is for supporting multiple ports, the other is for a slight compression.

Each USB packet contains:
- One byte that seems to increment every USB frame, then wraps around to zero.
- One byte that seems to always be zero.
- One mask byte, each bit set in the byte corresponds to an input port on the device.
- Data bytes, one for each bit set in the mask byte. Maximum 8 data bytes.

For every input port selected in the mask byte, follows one data byte.
So, number of bits set in the mask byte equals
the number of data bytes that follow the mask byte.
Repeat mask byte + data bytes until packet ends.
Sometimes the mask byte is zero, which is fine,
it just means there are no data bytes after  it,
so the next byte after it is another mask byte.

MIDI events with the same command (for example 93 for KEY ON on channel 4)
skip the command byte from the second event onward.
For example, if the original data is:
```93 10 7f 93 20 7f 93 10 00 93 20 00 fe```
The bytes that will be sent to the host will be:
```93 10 7f 20 7f 10 00 20 00 fe```
Thus, the driver must know the number of bytes that a MIDI event takes.

Thanks:
-------

* lost-bit0 for micro lite, micro express and XT support, and fixing a kernel panic
