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

## GPU usage stats

The driver counts how long V3D runs jobs on each of its three queues: `bin` and `render` (GL) and
`compute` (the ioctls above). Binning of one GL job can overlap with rendering of the previous one,
so `bin` and `render` can both be busy at the same time. A compute job always runs alone.

**Per process**, in `/proc/<pid>/fdinfo/<fd>` of each open `/dev/dri/card0` or `renderD128`, in the
standard format of
[drm-usage-stats](https://docs.kernel.org/gpu/drm-usage-stats.html) (which `gputop` reads):

```
drm-driver:	vc4
drm-client-id:	12
drm-engine-bin:	    1234567 ns
vc4-jobs-bin:	    42 jobs
drm-engine-render:	...
vc4-jobs-render:	...
drm-engine-compute:	...
vc4-jobs-compute:	...
drm-total-memory:	8192 KiB
drm-shared-memory:	0
drm-active-memory:	0
drm-resident-memory:	8192 KiB
drm-purgeable-memory:	0
```

- `drm-engine-*` is the total run time of the client's jobs, including the one running right now.
  The busy percentage is the increase between two reads divided by the wall-clock time between them.
  It measures time, not how many QPUs a job uses: a compute job on 1 of the 12 QPUs still counts as
  100 % busy.
- One process can have several DRM files open (for example a GL context and VC4CL), and a file
  shared between processes (fork, fd passing) shows up in each of them. Deduplicate by
  `drm-client-id`.
- The memory counts the BOs the file has a handle to. `drm-shared-memory` is BOs with handles in more
  than one file (for example buffers imported by the compositor). Firmware memory (`gpu_mem`) isn't
  included.
- The fdinfo of another user's process can only be read by root.

**For the whole device**, including clients that have closed, in
`/sys/class/drm/card0/device/gpu_stats` (world-readable, the format of the v3d driver's file of the
same name):

```
queue	timestamp	jobs	runtime
bin	123456789000	1000	2345678
render	123456789000	1000	34567890
compute	123456789000	10	4567890
```

`timestamp` and `runtime` are in ns of `CLOCK_MONOTONIC`.

The cost is two `ktime_get_ns()` calls per job while `job_lock` is already held. Reading the stats
doesn't touch the hardware, so it doesn't wake V3D from runtime suspend.
