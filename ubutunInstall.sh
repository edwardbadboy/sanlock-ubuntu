#!/bin/bash
echo "prerequisites: libaio-dev libblkid-dev"
echo "press ENTER to continue"
read DISCARDDDDDD
(cd wdmd
make && sudo DESTDIR=/ make install) && \
(cd src
make && sudo DESTDIR=/ make install) && \
(cd python
make && sudo DESTDIR=/ make install)
[ $? -eq 0 ] || exit 1

getent group sanlock >/dev/null || sudo groupadd -r sanlock
[ $? -eq 0 ] || exit 1

getent passwd sanlock > /dev/null || \
	sudo /usr/sbin/useradd -r sanlock \
	-c "sanlock daemon user" -d /var/run/sanlock -s /usr/sbin/nologin
[ $? -eq 0 ] || exit 1

sudo mkdir -p /var/run/sanlock && \
sudo chown sanlock:sanlock /var/run/sanlock && \
sudo chmod ug+rw /var/run/sanlock
[ $? -eq 0 ] || exit 1

install -m 755 sanlock.init /etc/init.d/sanlock
install -m 755 wdmd.init /etc/init.d/wdmd

sudo bash -c "echo 'enabled=1' >>/etc/default/sanlock"
sudo bash -c "echo 'sanlock_opts=\"-w 0 -G sanlock -U sanlock\"' >>/etc/default/sanlock"
sudo bash -c "echo 'wdmd_opts=\"-G sanlock\"' >>/etc/default/wdmd"
sudo service wdmd restart
sudo service sanlock restart
