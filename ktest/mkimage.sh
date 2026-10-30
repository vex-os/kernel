#!/bin/sh
# Usage: mkimage.sh <output> <limine-efi> <limine.conf> <kernel> [<file>:<path>]...
# Creates a GPT disk image with a FAT16 ESP containing the bootloader,
# its configuration and the kernel binary, an ext2 DATA partition, and a FAT16
# FAT partition that the tests of the FAT driver can write to.
# Each <file>:<path> puts a file at <path> of the DATA partition

export LANG=C.UTF-8
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

if test $# -lt 4; then
    printf "usage: %s <output> <limine-efi> <limine.conf> <kernel> [<file>:<path>]...\n" "${0}" >&2
    exit 1
fi

output="${1}"
limine_efi="${2}"
limine_conf="${3}"
kernel="${4}"

shift 4

temp_file="${output}.tmp"
data_root="${output}.root"

trap 'rm -rf "${temp_file}" "${data_root}"' EXIT

dd if=/dev/zero of="${temp_file}" bs=1M count=40 status=none || exit 1

sfdisk --quiet --force "${temp_file}" > /dev/null << SFDISK || exit 1
label: gpt
start=2048,size=16384,type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B,name=ESP
start=18432,size=53248,type=0FC63DAF-8483-4772-8E79-3D69D8477DE4,name=DATA
start=71680,size=8192,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,name=FAT
SFDISK

# The block count is in KiB and must match the ESP size above,
# otherwise mkfs.fat spans the filesystem to the end of the image
mkfs.fat -F16 -s 1 --offset 2048 "${temp_file}" $((16384 / 2)) > /dev/null || exit 1
mkfs.fat -F16 -s 1 --offset 71680 "${temp_file}" $((8192 / 2)) > /dev/null || exit 1

# Test tree for the ext2 driver: big.bin reaches the double indirect block,
# many/ spans several directory blocks, longlink doesn't fit into i_block
rm -rf "${data_root}" || exit 1
mkdir -p "${data_root}/dir/nested" "${data_root}/empty" "${data_root}/many" || exit 1
printf "Hello from ext2\n" > "${data_root}/hello.txt" || exit 1
printf "nested\n" > "${data_root}/dir/nested/file.txt" || exit 1
ln -s dir/nested/file.txt "${data_root}/link" || exit 1
ln -s dir/nested/../nested/../nested/../nested/../nested/../nested/file.txt "${data_root}/longlink" || exit 1
yes 0123456789abcde | head -c $((5 * 1024 * 1024)) > "${data_root}/big.bin" || exit 1
printf "end\n" | dd of="${data_root}/sparse.bin" bs=1 seek=$((1024 * 1024)) status=none || exit 1

# The mount point of devfs, and the scratch space of the test programs
mkdir -p "${data_root}/dev" "${data_root}/tmp" || exit 1

for entry in "$@"; do
    mkdir -p "${data_root}$(dirname "${entry#*:}")" || exit 1
    cp "${entry%%:*}" "${data_root}${entry#*:}" || exit 1
done

index=0
while test ${index} -lt 200; do
    printf "%d\n" ${index} > "${data_root}/many/entry_with_a_long_name_${index}" || exit 1
    index=$((index + 1))
done

# Features are spelled out so the result doesn't depend on the host's mke2fs.conf
mke2fs -q -F -t ext2 -b 4096 -I 256 -O none,filetype,sparse_super,large_file,ext_attr,resize_inode,dir_index \
    -E offset=$((18432 * 512)),root_owner=0:0 -d "${data_root}" "${temp_file}" $((53248 / 2))k || exit 1

esp="${temp_file}@@2048s"

mmd -i "${esp}" ::/EFI ::/EFI/BOOT || exit 1
mcopy -i "${esp}" "${limine_efi}" "::/EFI/BOOT/$(basename "${limine_efi}")" || exit 1
mcopy -i "${esp}" "${limine_conf}" ::/limine.conf || exit 1
mcopy -i "${esp}" "${kernel}" ::/vexsys.elf || exit 1

fat="${temp_file}@@71680s"

mmd -i "${fat}" ::/DIR || exit 1
printf "Hello from FAT\n" | mcopy -i "${fat}" - ::/a-rather-long-file-name.txt || exit 1

mv -f "${temp_file}" "${output}" || exit 1

exit 0
