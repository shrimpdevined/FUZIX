#!/bin/sh
set -e

ROOTIMG=rootfs.img
HOMEIMG=homefs.img
IMG=filesystem.img

# Internal QSPI layout (see config.h):
#   hda1  firmware root filesystem: essential OS and commands
#   hda2  user filesystem, mounted at /home
#   hda3  swap partition (256 KB), enabled at boot by /bin/swapon
# The FTL presents 2,402 logical sectors; the disk image uses 2,384 and
# leaves the rest as journal headroom.
ROOT_START=8
ROOT_BLOCKS=1344
HOME_START=1352
HOME_BLOCKS=520
SWAP_START=1872
SWAP_BLOCKS=512
DISK_BLOCKS=2384

# The root filesystem is built per board, so name the machine after the
# hardware this image (and its kernel) is being built for.
HOSTNAME=RP2040
case "${SUBTARGET:-pico}" in
pico2*)
	HOSTNAME=RP2350
	;;
esac
printf '%s' "$HOSTNAME" > hostname.tmp
trap 'rm -f hostname.tmp' EXIT

rm -f ${ROOTIMG} ${HOMEIMG} ${IMG}
# A larger inode table leaves room for user-created files now that the
# application store no longer consumes root-filesystem inodes.
../../../Standalone/mkfs ${ROOTIMG} 64 ${ROOT_BLOCKS}
../../../Standalone/ucp ${ROOTIMG} <<EOF
cd /
mkdir bin
mkdir dev
mkdir etc
mkdir mnt
mkdir mnt/sd
mkdir root
mkdir tmp
mkdir usr
mkdir var
chmod 0755 /
chmod 0755 bin
chmod 0755 dev
chmod 0755 etc
chmod 01777 tmp
chmod 0755 usr
chmod 0755 var
chmod 0700 root

mkdir /usr/man
mkdir /usr/man/man1
mkdir /var/run
mkdir /var/run/sudo
mkdir /var/log
mkdir /home
chmod 0700 /var/run/sudo
chmod 0755 /var/log
chmod 0755 /home

cd /usr
mkdir lib
chmod 0755 lib

cd /
cd /dev
mknod tty   20666 512
mknod tty1  20660 513
mknod tty2  20660 514
mknod tty3  20660 515
mknod tty4  20660 516
mknod tty5  20660 517
mknod tty6  20660 518
mknod tty7  20660 519
mknod tty8  20660 520
mknod hda   60660 0
mknod hda1  60660 1
mknod hda2  60660 2
mknod hda3  60660 3
mknod hda4  60660 4
mknod hda5  60660 5
mknod hda6  60660 6
mknod hda7  60660 7
mknod hda8  60660 8
mknod hda9  60660 9
mknod hda10 60660 10
mknod hda11 60660 11
mknod hda12 60660 12
mknod hda13 60660 13
mknod hda14 60660 14
mknod hda15 60660 15
mknod hdb   60660 16
mknod hdb1  60660 17
mknod hdb2  60660 18
mknod null  20666 1024
mknod kmem  20660 1025
mknod zero  20444 1026
mknod proc  20666 1027
mknod mem   20660 1028
mknod rtc   20600 1029
mknod sys   20644 1030
mknod i2c   20600 1031
mknod gpio  20600 1032
mknod fb0   20666 2048
mknod touch 20666 2304

cd /
bget ../../../Applications/util/init init
chmod 755 init

cd /etc
bget ../../../Standalone/filesystem-src/etc-files/fstab
bget etc-group group
bget ../../../Standalone/filesystem-src/etc-files/inittab
bget ../../../Standalone/filesystem-src/etc-files/motd
bget ../../../Standalone/filesystem-src/etc-files/issue
bget ../../../Standalone/filesystem-src/etc-files/mtab
bget etc-passwd passwd
bget etc-shadow shadow
# First-boot provisioning marker.  firstboot(1) removes it after the
# administrator account, password and home skeleton have been created.
bget firstboot-marker firstboot
bget rc
bget ../../../Standalone/filesystem-src/etc-files/rc.halt
bget ../../../Standalone/filesystem-src/etc-files/rc.reboot
bget ../../../Standalone/filesystem-src/etc-files/termcap
bget hostname.tmp hostname
chmod 0644 fstab
chmod 0644 firstboot
chmod 0644 group
chmod 0644 hostname
chmod 0644 inittab
chmod 0644 issue
chmod 0644 motd
chmod 0644 mtab
chmod 0644 passwd
chmod 0600 shadow
chmod 0644 termcap
chmod 0755 rc
chmod 0755 rc.halt
chmod 0755 rc.reboot

cd /root
bget root-profile .profile
chmod 0644 .profile

cd /bin
bget ../../../Applications/util/basename
bget ../../../Applications/util/blkdiscard
bget ../../../Applications/util/cat
bget ../../../Applications/util/chgrp
bget ../../../Applications/util/chmod
bget ../../../Applications/util/chown
bget ../../../Applications/util/cksum
bget ../../../Applications/util/cmp
bget ../../../Applications/util/cp
bget ../../../Applications/util/cut
bget ../../../Applications/util/date
bget ../../../Applications/util/dd
bget ../../../Applications/util/df
bget ../../../Applications/util/dirname
bget ../../../Applications/util/du
bget ../../../Applications/util/echo
bget ../../../Applications/util/ed
bget ../../../Applications/util/env
bget ../../../Applications/util/false
bget ../../../Applications/util/fdisk
bget ../../../Applications/util/fgrep
bget ../../../Applications/util/free
bget ../../../Applications/util/fsck
bget ../../../Applications/util/fsck-fuzix
bget ../../../Applications/util/grep
bget ../../../Applications/util/head
bget ../../../Applications/util/id
bget ../../../Applications/util/kill
bget ../../../Applications/util/killall
bget ../../../Applications/util/logname
bget ../../../Applications/util/ls
bget ../../../Applications/util/mkdir
bget ../../../Applications/util/mkfat
bget ../../../Applications/util/mkfs
bget ../../../Applications/util/mkfifo
bget ../../../Applications/util/mknod
bget ../../../Applications/util/more
bget ../../../Applications/util/mount
bget ../../../Applications/util/od
bget ../../../Applications/util/pagesize
bget ../../../Applications/util/passwd
bget ../../../Applications/util/printenv
bget ../../../Applications/util/prtroot
bget ../../../Applications/util/ps
bget ../../../Applications/util/pwd
bget ../../../Applications/util/reboot
bget ../../../Applications/util/remount
bget ../../../Applications/util/rm
bget ../../../Applications/util/rmdir
bget ../../../Applications/util/setdate
bget ../../../Applications/util/setboot
bget ../../../Applications/util/sleep
bget ../../../Applications/util/sort
bget ../../../Applications/util/stty
bget ../../../Applications/util/substroot
bget ../../../Applications/util/sum
bget ../../../Applications/util/su
bget ../../../Applications/util/sudo
bget ../../../Applications/util/swapon
chmod 0755 swapon
bget ../../../Applications/util/firstboot
bget ../../../Applications/util/sync
bget ../../../Applications/util/tar
bget ../../../Applications/util/tee
bget ../../../Applications/util/tail
bget ../../../Applications/util/telinit
bget ../../../Applications/util/touch
bget ../../../Applications/util/tr
bget ../../../Applications/util/true
bget ../../../Applications/util/umount
bget ../../../Applications/util/uname
bget ../../../Applications/util/uniq
bget ../../../Applications/util/uptime
bget ../../../Applications/util/wc
bget ../../../Applications/util/which
bget ../../../Applications/util/who
bget ../../../Applications/util/whoami
bget ../../../Applications/util/xargs
bget ../../../Applications/util/xrecv
bget ../../../Applications/util/yes

chmod 0755 basename
chmod 0755 blkdiscard
chmod 0755 cat
chmod 0755 chgrp
chmod 0755 chmod
chmod 0755 chown
chmod 0755 cksum
chmod 0755 cmp
chmod 0755 cp
chmod 0755 cut
chmod 0755 date
chmod 0755 dd
chmod 0755 df
chmod 0755 dirname
chmod 0755 du
chmod 0755 echo
chmod 0755 ed
chmod 0755 env
chmod 0755 false
chmod 0755 fdisk
chmod 0755 fgrep
chmod 0755 free
chmod 0755 fsck
chmod 0755 fsck-fuzix
chmod 0755 grep
chmod 0755 head
chmod 0755 id
chmod 0755 kill
chmod 0755 killall
chmod 0755 logname
chmod 0755 ls
chmod 0755 mkdir
chmod 0755 mkfat
chmod 0755 mkfs
chmod 0755 mkfifo
chmod 0755 mknod
chmod 0755 more
chmod 0755 mount
chmod 0755 od
chmod 0755 pagesize
chmod 4755 passwd
chmod 0755 printenv
chmod 0755 prtroot
chmod 0755 ps
chmod 0755 pwd
chmod 0755 reboot
chmod 0755 remount
chmod 0755 rm
chmod 0755 rmdir
chmod 0755 setdate
chmod 0775 setboot
chmod 0755 sleep
chmod 0755 sort
chmod 0755 stty
chmod 0755 substroot
chmod 0755 sum
chmod 4755 su
chmod 4755 sudo
chmod 0755 firstboot
chmod 0755 sync
chmod 0755 tar
chmod 0755 tee
chmod 0755 tail
chmod 0755 telinit
chmod 0755 touch
chmod 0755 tr
chmod 0755 true
chmod 0755 umount
chmod 0755 uname
chmod 0755 uniq
chmod 0755 uptime
chmod 0755 wc
chmod 0755 which
chmod 0755 who
chmod 0755 whoami
chmod 0755 xargs
chmod 0755 xrecv
chmod 0755 yes
ln cp mv
ln cp ln
ln reboot halt
ln reboot shutdown

bget ../../../Applications/V7/cmd/sh/sh
chmod 0755 sh

bget ../../../Applications/V7/cmd/dc
bget ../../../Applications/V7/cmd/test
bget ../../../Applications/V7/cmd/tty
chmod 0755 dc
chmod 0755 test
chmod 0755 tty


cd /usr/lib
bget ../../../Library/libs/liberror.txt
chmod 0644 liberror.txt

EOF

../../../Standalone/fsck -a ${ROOTIMG}

# The user filesystem gets a small inode table; firstboot creates
# the chosen administrator's home skeleton on first boot.
../../../Standalone/mkfs ${HOMEIMG} 48 ${HOME_BLOCKS}
../../../Standalone/ucp ${HOMEIMG} <<EOF
cd /
chmod 0755 /

EOF
../../../Standalone/fsck -a ${HOMEIMG}

# Compose the partitioned internal disk: MBR, firmware root, user home.
python3 - "${IMG}" "${ROOTIMG}" "${HOMEIMG}" \
	"${ROOT_START}" "${ROOT_BLOCKS}" \
	"${HOME_START}" "${HOME_BLOCKS}" \
	"${SWAP_START}" "${SWAP_BLOCKS}" "${DISK_BLOCKS}" <<'PY'
import struct
import sys

(img, root, home, root_start, root_blocks,
 home_start, home_blocks, swap_start, swap_blocks,
 disk_blocks) = sys.argv[1:]
root_start = int(root_start)
home_start = int(home_start)
swap_start = int(swap_start)
swap_blocks = int(swap_blocks)
disk_blocks = int(disk_blocks)

mbr = bytearray(512)

def partition(index, start, count):
    off = 446 + index * 16
    mbr[off:off + 4] = b'\x00\xfe\xff\xff'      # not bootable, LBA CHS
    mbr[off + 4:off + 8] = b'\x83\xfe\xff\xff'  # Linux data, LBA CHS
    struct.pack_into('<II', mbr, off + 8, start, count)

partition(0, root_start, int(root_blocks))
partition(1, home_start, int(home_blocks))
partition(2, swap_start, swap_blocks)
struct.pack_into('<H', mbr, 510, 0xAA55)

with open(img, 'wb') as out:
    out.truncate(disk_blocks * 512)
    out.seek(0)
    out.write(mbr)
    for path, start in ((root, root_start), (home, home_start)):
        out.seek(start * 512)
        with open(path, 'rb') as src:
            out.write(src.read())
PY

# ---------------------------------------------------------------------------
# SD application store
#
# The firmware image contains only the OS and the commands required to boot,
# authenticate, repair and use the machine.  Everything optional is staged
# here under /fuzix, matching the layout devfat provisions at mount time.
# ---------------------------------------------------------------------------
SDROOT=sdcard
rm -rf "$SDROOT"
mkdir -p "$SDROOT/fuzix/bin" "$SDROOT/fuzix/games" \
         "$SDROOT/fuzix/share/man/man1"

sdput() {
	dst="$SDROOT/fuzix/$2"
	cp "$1" "$dst"
	chmod 0755 "$dst"
}

sddoc() {
	dst="$SDROOT/fuzix/$2"
	cp "$1" "$dst"
	chmod 0644 "$dst"
}

sdput ../../../Applications/fcc/fcc bin/fcc
sdput ../../../Applications/fcc/cpp bin/cpp
sdput ../../../Applications/fcc/cc bin/cc
sdput ../../../Applications/fcc/runbin bin/runbin
sdput ../../../Applications/util/man bin/man
sdput ../../../Applications/util/marksman bin/marksman
sdput ../../../Applications/util/ll bin/ll
sdput utils/picoctl bin/picoctl
sdput ../../../Applications/util/banner bin/banner
sdput ../../../Applications/util/bd bin/bd
sdput ../../../Applications/util/cal bin/cal
sdput ../../../Applications/util/decomp16 bin/decomp16
sdput ../../../Applications/util/dosread bin/dosread
sdput ../../../Applications/util/fforth bin/fforth
sdput ../../../Applications/util/ssh bin/ssh
sdput ../../../Applications/util/swapon bin/swapon
sdput ../../../Applications/util/uud bin/uud
sdput ../../../Applications/util/uue bin/uue
sdput ../../../Applications/util/write bin/write
sdput ../../../Applications/V7/cmd/ac bin/ac
sdput ../../../Applications/V7/cmd/accton bin/accton
sdput ../../../Applications/V7/cmd/at bin/at
sdput ../../../Applications/V7/cmd/atrun bin/atrun
sdput ../../../Applications/V7/cmd/col bin/col
sdput ../../../Applications/V7/cmd/comm bin/comm
sdput ../../../Applications/V7/cmd/cron bin/cron
sdput ../../../Applications/V7/cmd/crypt bin/crypt
sdput ../../../Applications/V7/cmd/deroff bin/deroff
sdput ../../../Applications/V7/cmd/diff bin/diff
sdput ../../../Applications/V7/cmd/diff3 bin/diff3
sdput ../../../Applications/V7/cmd/diffh bin/diffh
sdput ../../../Applications/V7/cmd/join bin/join
sdput ../../../Applications/V7/cmd/look bin/look
sdput ../../../Applications/V7/cmd/mesg bin/mesg
sdput ../../../Applications/V7/cmd/newgrp bin/newgrp
sdput ../../../Applications/V7/cmd/pg bin/pg
sdput ../../../Applications/V7/cmd/pr bin/pr
sdput ../../../Applications/V7/cmd/ptx bin/ptx
sdput ../../../Applications/V7/cmd/rev bin/rev
sdput ../../../Applications/V7/cmd/split bin/split
sdput ../../../Applications/V7/cmd/time bin/time
sdput ../../../Applications/V7/cmd/tsort bin/tsort
sdput ../../../Applications/V7/cmd/wall bin/wall
sdput ../../../Applications/levee/levee bin/levee
sdput ../../../Applications/games/cowsay games/cowsay
sdput ../../../Applications/cave/advent games/advent
sddoc ../../../Applications/cave/advent.db games/advent.db
sddoc ../../../Applications/levee/levee.1 share/man/man1/levee.1

cat > "$SDROOT/README" <<'EOT'
FUZIX SD application store

The FUZIX kernel and its essential userland live in the Pico's internal
flash, so the machine boots and is usable without this card.  This card
only carries optional software:

  fuzix/bin              compiler, editors, extra commands
  fuzix/games            games and game data
  fuzix/share/man         additional manual pages

The kernel creates these directories at mount time and init adds
fuzix/bin and fuzix/games to PATH.  Copy the tree to the FAT partition
with a normal file manager, or from FUZIX with tar and xrecv.
EOT

echo "SD application tree staged in $PWD/$SDROOT"
