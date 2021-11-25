MOTU midi express 128 linux driver
==================================

Only tested on linux 5.x, and with midi express 128, the 5 port version might work too.

Build:
------

Make sure you have linux headers or linux source.

```bash
sudo apt-get install linux-headers-`uname -r`
```

```bash
make -C /lib/modules/`uname -r`/build M=$PWD
make -C /lib/modules/`uname -r`/build M=$PWD modules_install
```
