#!/bin/sh
# Copyright (c) 2022, 2023, 2024, 2025 Jonas 'Sortie' Termansen.
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
# installtest.sh
# Test an .iso release can be installed and upgraded.

set -e

all=false
unset bios
unset display
display="-display none"
unset enable_kvm
harddisk_size=4G
firmware=bios
unset host
unset hostname
hostname=installtest
install=false
minimal=false
network_upgrade=false
unset input
unset iso
iso_upgrade=false
unset output
partitioning=gpt
unset port
unset qemu
unset release
unset release_key
unset release_url
source_upgrade=false
unset version
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
  --all) all=true ;;
  --bios) previous_option=bios ;;
  --bios=*) bios=$parameter ;;
  --display) previous_option=display ;;
  --display=*) display=$parameter ;;
  --enable-kvm) enable_kvm=-enable-kvm ;;
  --firmware) previous_option=firmware ;;
  --firmware=*) firmware=$parameter ;;
  --harddisk-size) previous_option=harddisk_size ;;
  --harddisk-size=*) harddisk_size=$parameter ;;
  --host) previous_option=host ;;
  --host=*) host=$parameter ;;
  --hostname) previous_option=hostname ;;
  --hostname=*) hostname=$parameter ;;
  --input) previous_option=input ;;
  --input=*) input=$parameter ;;
  --install) install=true ;;
  --iso) previous_option=iso ;;
  --iso=*) iso=$parameter ;;
  --iso-upgrade) iso_upgrade=true ;;
  --minimal) minimal=true ;;
  --network-upgrade) network_upgrade=true ;;
  --output) previous_option=output ;;
  --output=*) output=$parameter ;;
  --partitioning) previous_option=partitioning ;;
  --partitioning=*) partitioning=$parameter ;;
  --port) previous_option=port ;;
  --port=*) port=$parameter ;;
  --qemu) previous_option=qemu ;;
  --qemu=*) qemu=$parameter ;;
  --release) previous_option=release ;;
  --release=*) release=$parameter ;;
  --release-key) previous_option=release_key ;;
  --release-key=*) release_key=$parameter ;;
  --release-url) previous_option=release_url ;;
  --release-url=*) release_url=$parameter ;;
  --source-upgrade) source_upgrade=true ;;
  --version) previous_option=version ;;
  --version=*) version=$parameter ;;
  --vm-root) previous_option=vm_root ;;
  --vm-root=*) vm_root=$parameter ;;
  -*) echo "$0: unrecognized option $argument" >&2
      exit 1 ;;
  *)
    echo "$0: unexpected extra operand '$argument'" >&2
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

case "$bios" in "" | /*) ;; *) bios="$PWD/$bios" ;; esac
case "$iso" in "" | /*) ;; *) iso="$PWD/$iso" ;; esac
case "$input" in "" | /*) ;; *) input="$PWD/$input" ;; esac
case "$output" in "" | /*) ;; *) output="$PWD/$input" ;; esac

if $all; then
  mkdir -p "$input"
  mkdir -p "$output"

  set -x

  if [ "$version" = "$release" ]; then
    prefix="$version"
  else
    prefix="$version-$release"
  fi

  new_stable=${version%-*}
  new_stable_regexp=^$(echo "$new_stable" | sed -E 's/\./\\./g')$
  version_regexp=^$(echo "$version" | sed -E 's/\./\\./g')$
  old_stable=$( (find "$input" -name '*.release' -exec cat '{}' ';' |
                 grep -Ev -- - |
                 grep -Ev "$new_stable_regexp";
                 echo "$version") |
                sort -Vu | grep -E "$version_regexp" -B1 | tail -2 | head -1)

  # Test installation.
  "$0" ${enable_kvm+--enable-kvm} --host="$host" --port="$port" --iso="$iso" \
       ${display+--display="$display"} --bios="$bios" --firmware=bios \
       --install --partitioning=gpt --output="$output/$prefix-$host.hdd"

  # Test minimal installation.
  "$0" ${enable_kvm+--enable-kvm} --host="$host" --port="$port" --iso="$iso" \
       ${display+--display="$display"} --bios="$bios" --firmware=bios \
       --install --partitioning=gpt --minimal \
       --output="$output/$prefix-$host-minimal.hdd"

  # Test upgrading an the previous stable to the current release.
  if [ -e "$input/$old_stable-$host.hdd" ]; then
    old_stable_input="$input/$old_stable-$host.hdd"
  else
    old_stable_input="$output/$prefix-$host.hdd"
  fi
  "$0" ${enable_kvm+--enable-kvm} --host="$host" --port="$port" --iso="$iso" \
       ${display+--display="$display"} --bios="$bios" --iso-upgrade \
       --input="$old_stable_input"

  # Test upgrading from network.
  network_input="$input/$prefix-$host-network.hdd"
  if [ ! -e "$network_input" ]; then
     network_input="$output/$prefix-$host-minimal.hdd"
  fi
  "$0" ${enable_kvm+--enable-kvm} --host="$host" --port="$port" --iso="$iso" \
       ${display+--display="$display"} --bios="$bios" --network-upgrade \
       --release-key="$release_key" --release-url="$release_url" \
       --input="$network_input" --output="$output/$prefix-$host-network.hdd"

  # Test upgrading from source.
  if [ -e "$input/$prefix-$host.hdd" ]; then
    previous_input="$input/$prefix-$host.hdd"
  else
    previous_input="$output/$prefix-$host.hdd"
  fi
  "$0" ${enable_kvm+--enable-kvm} --host="$host" --port="$port" --iso="$iso" \
       ${display+--display="$display"} --bios="$bios" --source-upgrade \
       --release-key="$release_key" --release-url="$release_url" \
       --input="$previous_input"

  echo "$version" > "$output/$prefix.release"

  exit
fi

if ! $install && ! $iso_upgrade && ! $network_upgrade && ! $source_upgrade; then
  install=true
fi

if $minimal; then
  memory=300
elif $network_upgrade; then
  memory=256
elif $source_upgrade; then
  memory=512
else
  memory=1280
fi

if [ -z "$qemu" ]; then
  case "$host" in
  i[3456]86-*) qemu=qemu-system-i386;;
  x86_64-*) qemu=qemu-system-x86_64;;
  *) echo "$0: error: Unable to detect qemu program for: $HOST" >&2
     exit 1
  esac
fi

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

set -x

# Create the installation harddisk
if [ -n "$input" ]; then
  cp "$input" sortix.hdd
  cp "$input.firmware" sortix.hdd.firmware
  firmware=$(cat sortix.hdd.firmware)
  # TODO: After releasing Sortix 1.1, remove this hack for Sortix 1.0 compat.
  if ! $iso_upgrade; then
    cp "$input.known_hosts" sortix.hdd.known_hosts
    cp "$input.id_rsa" sortix.hdd.id_rsa
    cp "$input.id_rsa.pub" sortix.hdd.id_rsa.pub
  fi
else
  if ! $install; then
    echo "$0: error: --input must be used when --install is not used"
    exit 1
  fi
  qemu-img create -f qcow2 sortix.hdd $harddisk_size
  echo "$firmware" > sortix.hdd.firmware
fi
if [ "$firmware" != efi ]; then
  unset bios
fi

# Configure the ssh authentication and enable sshd.
if $install || $iso_upgrade; then
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
fi

# Enable automatic installation.
if $install; then
cat >> liveconfig/etc/autoinstall.conf << EOF
accept_defaults=yes
countdown=0
grub=yes
EOF
if [ $partitioning = mbr ]; then
  echo "disked++=mktable mbr" >> liveconfig/etc/autoinstall.conf
elif [ $partitioning = gpt ]; then
  echo "disked++=mktable gpt" >> liveconfig/etc/autoinstall.conf
fi
if [ $partitioning = gpt -a $firmware = bios ]; then
  echo "disked++=mkpart 1 0% 1M biosboot" >> liveconfig/etc/autoinstall.conf
fi
if [ $firmware = efi ]; then
  echo "disked++=mkpart 1 0% 64M efi /boot/efi" >> liveconfig/etc/autoinstall.conf
fi
cat >> liveconfig/etc/autoinstall.conf << EOF
disked++=mkpart 1 0% 100% ext2 /
hostname=$HOSTNAME
enable_sshd=yes
password_hash_root=x
finally!++=sed -Ei -- "s/src = yes/src = no/" etc/upgrade.conf
finally!++=chroot -d . sh -c 'echo GRUB_TIMEOUT=0 >> etc/grub && update-grub' 
finally!++=echo exit
EOF
fi

# Enable automatic upgrading.
if $iso_upgrade; then
cat >> liveconfig/etc/autoupgrade.conf << EOF
accept_defaults=yes
countdown=0
# TODO: After releasing Sortix 1.1, remove support for the old disk image
#       without sshd support.
finally!++=cp -t etc /etc/ssh*
finally!++=cp -RT /root/.ssh root/.ssh
finally!++=echo require sshd optional >> etc/init/local
finally!++=echo exit
EOF
fi

# Customize the bootloader configuration.
if $install || $iso_upgrade; then
tix-iso-bootconfig \
  --random-seed \
  --timeout=0 \
  --disable-gui \
  --liveconfig=liveconfig \
  bootconfig
mkdir -p bootconfig/boot/grub
if "$minimal"; then
  cat >> bootconfig/boot/grub/hooks.cfg << EOF
select_ports_set_minimal
port_ssh=true
EOF
fi
tix-iso-add "$iso" bootconfig -o sortix.iso
fi

# Spawn the virtual machine in the background.
qemu() {
  exec $qemu \
    $enable_kvm \
    -no-reboot \
    ${bios+-bios "$bios"} \
    -name "$hostname" \
    -nodefaults \
    $display \
    -m $memory \
    -vga std \
    -hda sortix.hdd \
    -device e1000,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22 \
    "$@"
}
if $install || $iso_upgrade; then
  qemu -cdrom sortix.iso -boot d &
else
  qemu &
fi
vmpid=$!
echo $vmpid > pid

# Wait for the virtual machine to boot to sshd.
key=id_rsa
known_hosts=sortix.iso.known_hosts
do_ssh() {
  ssh \
    -i $key \
    -oUserKnownHostsFile=$known_hosts \
    -oConnectionAttempts=30 \
    -p $port \
    root@localhost \
    "$@"
}
wait_ssh() {
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
}

# Test if the release can be installed correctly.

if $network_upgrade || $source_upgrade; then
  key=sortix.hdd.id_rsa
  known_hosts=sortix.hdd.known_hosts
fi

wait_ssh
if $install; then
  do_ssh "sysinstall"
elif $iso_upgrade; then
  do_ssh "sysupgrade"
elif $network_upgrade; then
  if [ -n "$release_key" ]; then
    do_ssh 'cat > /tix/release.pub' < "$release_key"
  fi
  if [ -n "$release_url" ]; then
    do_ssh "tix-vars /tix/collection.conf RELEASE_URL=\"$release_url\" \\
            > /tix/collection.conf.new &&
            mv /tix/collection.conf.new /tix/collection.conf"
  fi
  do_ssh /sbin/tix-upgrade --cancel
  do_ssh /sbin/tix-upgrade --force
elif $source_upgrade; then
  xorriso -indev "$iso" -osirrox on -extract boot/src.tar.xz src.tar.xz
  do_ssh "rm -rf /src && tar -xJ -C /dev" < src.tar.xz
  rm -f src.tar.xz
  do_ssh "cd /dev/src && make install-build-tools && make clean-build-tools && make PACKAGES= sysmerge"
fi
do_ssh "poweroff" || true

wait $vmpid
unset vmpid
rm pid

if $install || $iso_upgrade; then
  cp sortix.iso.known_hosts sortix.hdd.known_hosts
  cp id_rsa sortix.hdd.id_rsa
  cp id_rsa.pub sortix.hdd.id_rsa.pub
fi
key=sortix.hdd.id_rsa
known_hosts=sortix.hdd.known_hosts

# Test if a release can network upgrade to itself after already upgrading.
if $network_upgrade; then

  qemu &
  vmpid=$!
  echo $vmpid > pid

  wait_ssh
  do_ssh /sbin/tix-upgrade --force
  do_ssh "poweroff" || true

  wait $vmpid
  unset vmpid
  rm pid

fi

# Test if the installation boots correctly.

qemu &
vmpid=$!
echo $vmpid > pid

wait_ssh
do_ssh "poweroff" || true

wait $vmpid
unset vmpid
rm pid

if [ -n "$output" ]; then
  cp sortix.hdd "$output"
  cp sortix.hdd.firmware "$output.firmware"
  cp sortix.hdd.known_hosts "$output.known_hosts"
  cp sortix.hdd.id_rsa "$output.id_rsa"
  cp sortix.hdd.id_rsa.pub "$output.id_rsa.pub"
fi
