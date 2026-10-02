# vc4-compute

The Raspberry Pi `vc4` DRM driver, patched to run QPU user programs (OpenCL kernels from VC4C/VC4CL) as
jobs scheduled by the driver. The design is in [../VC4CL/DRM-DESIGN.md](../VC4CL/DRM-DESIGN.md).

- `vc4/`: the driver from Raspberry Pi kernel commit `c1432b4bae5b`, which is the kernel of Raspberry
  Pi OS package `linux-image-6.6.31+rpt-rpi-v8` (`1:6.6.31-1+rpt1`). The first git commit is the
  unmodified driver, so `git diff <first commit>` shows the whole patch.
- `vc4/vc4_compute_drm.h`: the uAPI, shared with VC4CL.
- `vc4/vc4_compute.c`: most of the implementation.
- `tests/compute_test.c`: tests of the new ioctls with hand-assembled QPU programs.
- `kbuild-trace/`: only makes the tracepoint header resolvable in an out-of-tree build.

The module only works with the exact kernel above: `vermagic` must match `uname -r`.

## Build

```bash
cd vc4
make -C /lib/modules/$(uname -r)/build M=$PWD KCFLAGS="-I$PWD/../kbuild-trace/a/b" -j3 modules
```

## Install

`vc4.ko` is in the initramfs, so the stock driver is loaded before the root file system is mounted.
The patched module must be installed to `updates/`, which `depmod` prefers over `kernel/`, and the
initramfs must be regenerated:

```bash
sudo install -D -m 644 vc4/vc4.ko /lib/modules/$(uname -r)/updates/vc4.ko
sudo depmod -a
modinfo -n vc4                 # must print .../updates/vc4.ko
sudo update-initramfs -u -k $(uname -r)   # only this kernel; the -2712 flavour is for the Pi 5
sudo reboot
```

Afterwards, `tests/compute_test` should get past "does not support compute jobs".

## Uninstall (back to the stock driver)

```bash
sudo rm /lib/modules/$(uname -r)/updates/vc4.ko
sudo depmod -a
modinfo -n vc4                 # must print .../kernel/drivers/gpu/drm/vc4/vc4.ko.xz
sudo update-initramfs -u -k $(uname -r)   # only this kernel; the -2712 flavour is for the Pi 5
sudo reboot
```

If the patched module breaks the display, the Pi still boots and can be reached over SSH to run these
commands.

## Letting a group use compute without root

By default, the compute ioctls need `CAP_SYS_RAWIO`, so in practice root. The `compute_gid` module
parameter additionally allows the members of one group.

V3D has no MMU, so members of that group can read and write all physical memory: **they effectively
get root rights.** Only use a group whose members already have them. On Raspberry Pi OS, members of
`video` (GID 44) can already run code on the GPU through `/dev/vcio` and `/dev/vchiq`, which is how
VC4CL ran without root on the old firmware stack. So `video` adds nothing they couldn't already do.
Don't use `render`: it's meant to be safe for any GL client, and it includes service users such as
`vnc`.

Until the next reboot:

```bash
echo 44 | sudo tee /sys/module/vc4/parameters/compute_gid
```

Permanently: `vc4` loads from the initramfs, so the option has to go into it.

```bash
echo "options vc4 compute_gid=44" | sudo tee /etc/modprobe.d/vc4-compute.conf
sudo update-initramfs -u -k $(uname -r)
```

`-1` (the default) disables it again.

## Behavior changes besides the compute ioctls

- **V3D runtime-suspends when idle.** This is the fix backported from `rpi-6.12.y`, see DRM-DESIGN.md.
  It's what makes `vc4_reset()` actually power-cycle the GPU.
- **Don't set `/sys/bus/platform/devices/3fc00000.v3d/power/control` to `on`.** That blocks runtime
  suspend, so a timed-out compute job can't be reset, and compute ends up wedged. VC4CL's
  register-poking backend sets it to `on` while running (see `../VC4CL/KMS.md`), so don't use that
  backend together with this module.
