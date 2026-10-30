#!/bin/sh

export LANG=C.UTF-8
export LANGUAGE=C.UTF-8
export LC_ALL=C.UTF-8

cd $(dirname $(realpath $(dirname ${0})))

# Set by the ktest-run target in ktest/meson.build, defaults
# to the image in a build directory named "build"
VEX_IMAGE="${VEX_IMAGE:-build/ktest/vexsys.boot.img}"

if test -f "/proc/sys/fs/binfmt_misc/WSLInterop"; then
    UEFI_PATH=$(wslpath -a -w contrib/edk2/RELEASEX64_OVMF.fd)
    IMAGE_PATH=$(wslpath -a -w "${VEX_IMAGE}")
    QEMU_EXECUTABLE=qemu-system-x86_64.exe
    PATH_CONVERT="wslpath -a -w"
else
    UEFI_PATH=contrib/edk2/RELEASEX64_OVMF.fd
    IMAGE_PATH="${VEX_IMAGE}"
    QEMU_EXECUTABLE=qemu-system-x86_64
    PATH_CONVERT="realpath"
fi

logfile="qemu.log"

# The first serial port is the shell of the test runner on this terminal,
# the second one gets the kernel log. Ctrl-A X quits QEMU, Ctrl-A C opens its monitor
qargs=
qargs="${qargs} -bios ${UEFI_PATH}"
qargs="${qargs} -drive format=raw,file=${IMAGE_PATH}"
qargs="${qargs} -serial mon:stdio"
qargs="${qargs} -serial file:$(${PATH_CONVERT} kernel.log)"
qargs="${qargs} -machine type=q35"
qargs="${qargs} -cpu max"
qargs="${qargs} -vga std"
qargs="${qargs} -m 512M"
qargs="${qargs} -smp 4"

for arg in "$@"; do
    case "${arg}" in
        slow)    qargs="${qargs} -icount shift=0,sleep=on" ;;
        slow=*)  qargs="${qargs} -icount shift=${arg#slow=},sleep=on" ;;
        verbose) qargs="${qargs} -d cpu_reset,int -no-reboot -no-shutdown" ;;
        [0-9]*) logfile="qemu_${arg}.log" ;;
    esac
done

rm -f "${logfile}" kernel.log

# The terminal would otherwise edit the line and echo it before QEMU sees it, and the guest
# echoes it again. Ctrl-C goes to the guest, where ^C interrupts a test. Enter stays a
# newline (icrnl), as a lone carriage return never reaches the guest through QEMU's stdio
if test -t 0; then
    saved_terminal=$(stty -g)
    trap 'stty "${saved_terminal}"' EXIT
    stty -icanon -echo -isig -ixon
fi

{ ${QEMU_EXECUTABLE} ${qargs} 2>&1; printf "\n"; } | tee "${logfile}"

exit 0
