#!/bin/sh
# Copyright (c) 2022, 2023, 2024 Jonas 'Sortie' Termansen.
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
# smoketest.sh
# Test an .iso release boots and passes rudimentary tests.

set -e

unset display
display="-display none"
unset enable_kvm
unset host
unset hostname
hostname=smoketest
unset iso
unset port
unset qemu
unset vm_root

dashdash=
previous_option=
for argument do
  if [ -n "$previous_option" ]; then
    eval $previous_option=\$argument
    previous_option=
    continue
  fi

  case $argument in
  *=?*) parameter=$(expr "X$argument" : '[^=]*=\(.*\)' || true) ;;
  *=)   parameter= ;;
  *)    parameter=yes ;;
  esac

  case $dashdash$argument in
  --) dashdash=yes ;;
  --display=*) display=$parameter ;;
  --display) previous_option=display ;;
  --enable-kvm) enable_kvm=-enable-kvm ;;
  --host=*) host=$parameter ;;
  --host) previous_option=host ;;
  --hostname=*) hostname=$parameter ;;
  --hostname) previous_option=hostname ;;
  --iso=*) iso=$parameter ;;
  --iso) previous_option=iso ;;
  --port=*) port=$parameter ;;
  --port) previous_option=port ;;
  --qemu=*) qemu=$parameter ;;
  --qemu) previous_option=qemu ;;
  --vm-root=*) vm_root=$parameter ;;
  --vm-root) previous_option=vm_root ;;
  -*) echo "$0: unrecognized option $argument" >&2
      exit 1 ;;
  *)
    echo "$0: unexpected extra operand $argument" >&2
    exit 1
    ;;
  esac
done

if [ -n "$previous_option" ]; then
  echo "$0: option '$argument' requires an argument" >&2
  exit 1
fi

if [ -z "$host" ]; then
  echo "$0: error: No --host was specified" >&2
  exit 1
fi

if [ -z "$iso" ]; then
  echo "$0: error: No --iso was specified" >&2
  exit 1
fi
iso=$(realpath -- "$iso")

if [ -z "$port" ]; then
  echo "$0: error: No --port was specified" >&2
  exit 1
fi

if [ -z "$qemu" ]; then
  case "$host" in
  i[3456]86-*) qemu=qemu-system-i386;;
  x86_64-*) qemu=qemu-system-x86_64;;
  *) echo "$0: error: Unable to detect qemu program for: $HOST" >&2
     exit 1
  esac
fi

set -x

unset tmpdir
# Reclaim a persistent VM directory (if any) and kill any leaked processes.
if [ -n "$vm_root" ]; then
  unset tmpdir
  export TMPDIR="$vm_root/$hostname"
  mkdir -p -- "$TMPDIR"
  if [ -e "$TMPDIR/pid" ]; then
    kill -KILL "$(cat "$TMPDIR/pid")" || true
  fi
  rm -rf -- "$TMPDIR"/*
# Otherwise use a temporary directory that will be deleted upon completion.
else
  export TMPDIR=$(mktemp -d -t "$hostname.vm.XXXXXXXXX")
  tmpdir="$TMPDIR"
fi

# On exit, power off the virtual machine and clean up the working directory.
unset vmpid
cleanup_tmpdir() {
  if [ -n "$vmpid" ]; then
    kill -KILL $vmpid || true
  fi
  rm -f "$TMPDIR/pid"
  if [ -n "$tmpdir" ]; then
    rm -rf -- "$tmpdir"
  elif [ -n "$TMPDIR" ]; then
    rm -rf -- "$TMPDIR"/*
  fi
}
trap cleanup_tmpdir EXIT HUP INT QUIT
cd "$TMPDIR"

# Configure the ssh authentication and enabling sshd.
ssh-keygen -f id_rsa -N ''
rm -rf liveconfig bootconfig
tix-iso-liveconfig \
  --hostname="$hostname" \
  --root-ssh-authorized-keys=id_rsa.pub \
  --sshd-keygen \
  --sshd-key-known-hosts-file=sortix.iso.known_hosts \
  --sshd-key-known-hosts-hosts='localhost 127.0.0.1' \
  liveconfig
mkdir -p liveconfig/etc liveconfig/etc/init
cat > liveconfig/etc/init/local << EOF
require sshd optional
EOF
tix-iso-bootconfig \
  --random-seed \
  --timeout=0 \
  --disable-gui \
  --liveconfig=liveconfig \
  bootconfig
mkdir -p bootconfig/boot/grub
cat >> bootconfig/boot/grub/hooks.cfg << EOF
select_ports_set_no
port_binutils=true
port_dash=true
port_gcc=true
port_grep=true
port_make=true
port_sed=true
port_ssh=true
EOF
tix-iso-add "$iso" bootconfig -o sortix.iso

# Spawn the virtual machine in the background.
$qemu \
  $enable_kvm \
  -name "$hostname" \
  -nodefaults \
  $display \
  -m 440 \
  -vga std \
  -boot d \
  -cdrom sortix.iso \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22 &
vmpid=$!
echo $vmpid > pid

# Wait for the virtual machine to boot to sshd.
do_ssh() {
  ssh \
    -i id_rsa \
    -oUserKnownHostsFile=sortix.iso.known_hosts \
    -oConnectionAttempts=30 \
    -p $port \
    root@localhost \
    "$@"
}
retries=30
while true; do
  if ! do_ssh 'uname -a'; then
    if [ $retries = 1 ]; then
      exit 1
    fi
    sleep 1
    if ! kill -WINCH "$vmpid"; then
      unset vmpid
      rm -f "$TMPDIR/pid"
      exit 1
    fi
    retries=$(expr $retries - 1)
    continue
  fi
  break
done

# Test for smoke.
do_ssh 'regress'
do_ssh 'cd /src/editor && make'
do_ssh '! grep -Ei "failed|exited unsuccessfully" /var/log/init.log'
do_ssh 'memstat -a && df -h'
