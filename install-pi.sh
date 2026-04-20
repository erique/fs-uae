#!/usr/bin/env bash

if [ `id -u` != 0 ] ; then
	echo "Please run install-pi.sh using sudo (sudo ./install-pi.sh)"
	exit 1
fi

A314_SW=a314/Software
BIN=$A314_SW/bin_pi

A314_USER=$SUDO_USER
A314_GROUP=`sudo -u $A314_USER id -gn`
A314_HOME=`sudo -u $A314_USER printenv HOME`

install_common() {
	mkdir -p /opt/a314
	cp $A314_SW/a314d/a314d.py /opt/a314
	cp $A314_SW/picmd/picmd.py /opt/a314
	cp $A314_SW/a314fs/a314fs.py /opt/a314
	cp $A314_SW/piaudio/piaudio.py /opt/a314
	cp $A314_SW/remotewb/remotewb.py /opt/a314
	cp $A314_SW/disk/disk.py /opt/a314
	cp $A314_SW/ethernet/ethernet.py /opt/a314
	cp $A314_SW/hid/hid.py /opt/a314
	cp $A314_SW/remote-mouse/remote-mouse.py /opt/a314
	cp $A314_SW/videoplayer/videoplayer.py /opt/a314
	cp $A314_SW/usbhardware/usbbridge.py /opt/a314
	cp $A314_SW/usbhardware/usb_protocol.py /opt/a314

	mkdir -p /etc/opt/a314

#	# Write configuration files, but don't overwrite
	[ -f /etc/opt/a314/a314d.conf ] || cp $A314_SW/a314d/a314d.conf /etc/opt/a314
	[ -f /etc/opt/a314/picmd.conf  ] || cp $A314_SW/picmd/picmd.conf /etc/opt/a314
	[ -f /etc/opt/a314/a314fs.conf ] || cp $A314_SW/a314fs/a314fs.conf /etc/opt/a314
	[ -f /etc/opt/a314/disk.conf   ] || cp $A314_SW/disk/disk.conf /etc/opt/a314
	[ -f /etc/opt/a314/usbbridge.conf ] || cp $A314_SW/usbhardware/usbbridge.conf /etc/opt/a314

	# Install Python packages in virtual environment
	python3 -m venv /opt/a314/venv
	/opt/a314/venv/bin/pip install pyusb

	echo
	echo "Installation complete"
}

install_common
