# bluetooth-tests

Tests of bluez dbus interface for Bluetooth and BLE

## Build bluez 5.46

sudo apt-get install debhelper dh-autoreconf flex bison libdbus-glib-1-dev libglib2.0-dev libcap-ng-dev libudev-dev libreadline-dev libical-dev check dh-systemd libebook1.2-dev

wget https://launchpad.net/ubuntu/+archive/primary/+files/bluez_5.46.orig.tar.xz
wget https://launchpad.net/ubuntu/+archive/primary/+files/bluez_5.46-0ubuntu1.debian.tar.xz
wget https://launchpad.net/ubuntu/+archive/primary/+files/bluez_5.46-0ubuntu1.dsc

tar xf bluez_5.46.orig.tar.xz
cd bluez-5.46
tar xf ../bluez_5.46-0ubuntu1.debian.tar.xz
debchange --local=~lorenzen 'Backport to Xenial'
debuild -b -j4
cd ..
sudo dpkg -i *.deb


## What gets installed

bluetooth_5.46-0ubuntu1~lorenzen1_all.deb
bluez_5.46-0ubuntu1~lorenzen1_amd64.deb
bluez-cups_5.46-0ubuntu1~lorenzen1_amd64.deb
bluez-dbg_5.46-0ubuntu1~lorenzen1_amd64.deb
bluez-hcidump_5.46-0ubuntu1~lorenzen1_amd64.deb
bluez-obexd_5.46-0ubuntu1~lorenzen1_amd64.deb
bluez-tests_5.46-0ubuntu1~lorenzen1_amd64.deb
libbluetooth3_5.46-0ubuntu1~lorenzen1_amd64.deb
libbluetooth3-dbg_5.46-0ubuntu1~lorenzen1_amd64.deb
libbluetooth-dev_5.46-0ubuntu1~lorenzen1_amd64.deb


## Set up the bluez daemon to expose BLE on dbus

Edit
/etc/systemd/system/bluetooth.target.wants/bluetooth.service

From
ExecStart=/usr/libexec/bluetooth/bluetoothd
To
ExecStart=/usr/libexec/bluetooth/bluetoothd --experimental

## Others

Check Bluetooth adapter state
hciconfig hci0

