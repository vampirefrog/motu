#!/bin/bash
### debpkg.sh
###  by darkQ
### Generate a deb package with adapted dkms config
### Tested on : Linux Mint 22.2 (Ubuntu 24.04)
###
VERSION=1.0
SUBVER=$(date "+%Y %j" | awk '{printf("%02d%03d\n",$(1)-2000,$2);}')
PKGNAME=motu
DEBPATH=${PKGNAME}_${VERSION}-${SUBVER}
SRCPATH=/usr/src/${PKGNAME}-${VERSION}
DEBSRCP=${DEBPATH}${SRCPATH}

mkdir -p ${DEBPATH}/DEBIAN 2>/dev/null
mkdir -p ${DEBSRCP} 2>/dev/null

cp Makefile ${DEBSRCP}/
cp *.c ${DEBSRCP}/
cp dkms-deb.conf ${DEBSRCP}/dkms.conf

cat >${DEBPATH}/DEBIAN/control << EOF
Package: motu
Version: ${VERSION}-${SUBVER}
Section: base
Priority: optional
Architecture: all
Depends: dkms
Maintainer: darkQ
Description: Motu MIDI Express 128 USB driver by vampirefrog
EOF

cat >${DEBPATH}/DEBIAN/postinst <<EOF
dkms add -m $PKGNAME -v $VERSION
dkms build -m $PKGNAME -v $VERSION
dkms install -m $PKGNAME -v $VERSION
echo "motu" | /etc/modules-load.d/motu.conf
modprobe motu
EOF

cat >${DEBPATH}/DEBIAN/prerm <<EOF
modprobe -r motu
rm -rf /etc/modules-load.d/motu.conf
dkms uninstall -m $PKGNAME -v $VERSION
dkms unbuild -m $PKGNAME -v $VERSION
dkms remove -m $PKGNAME -v $VERSION
EOF

chmod 755 ${DEBPATH}/DEBIAN/postinst
chmod 755 ${DEBPATH}/DEBIAN/prerm

dpkg-deb --build $DEBPATH
