# qemu-obc

This is a qemu fork for OBC100 board used in the satella project.

## Build
```shell
git clone https://github.com/rota1001/qemu-obc.git --depth 1
cd qemu-obc
mkdir build && cd build
../configure --target-list=arm-softmmu \
    --without-default-devices --with-devices-arm=obc100 \
    --without-default-features \
    --disable-docs --disable-tools --disable-guest-agent --disable-tests \
    --enable-tcg --enable-fdt=internal
ninja qemu-system-arm
```
## Run
First build the firmware `TMS570LC43_OBC_PLUS_20240102.out`, and then run it:
```shell
truncate -s 16M sd.img
./qemu-system-arm -M obc100 -kernel ./TMS570LC43_OBC_PLUS_20240102.out \
  -nographic -serial stdio -no-reboot -drive if=sd,format=raw,file=sd.img
```
