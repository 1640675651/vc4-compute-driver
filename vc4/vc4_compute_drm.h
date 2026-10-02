/* SPDX-License-Identifier: MIT */
/*
 * Out-of-tree extension of the vc4 uAPI: running QPU user programs (e.g. OpenCL kernels) as jobs scheduled by the
 * vc4 driver, see VC4CL/DRM-DESIGN.md.
 *
 * Since the VideoCore IV has no MMU, a QPU program can access all of physical memory. All ioctls in here therefore
 * require CAP_SYS_RAWIO, or membership in the group set by the vc4 module parameter compute_gid (disabled by
 * default).
 */

#ifndef _UAPI_VC4_COMPUTE_DRM_H_
#define _UAPI_VC4_COMPUTE_DRM_H_

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Kept well away from the upstream vc4 ioctl numbers (0x00 - 0x0e) */
#define DRM_VC4_SUBMIT_QPU                        0x20
#define DRM_VC4_WAIT_QPU                          0x21
#define DRM_VC4_QPU_BO_ADDR                       0x22

#define DRM_IOCTL_VC4_SUBMIT_QPU          DRM_IOWR(DRM_COMMAND_BASE + DRM_VC4_SUBMIT_QPU, struct drm_vc4_submit_qpu)
#define DRM_IOCTL_VC4_WAIT_QPU            DRM_IOWR(DRM_COMMAND_BASE + DRM_VC4_WAIT_QPU, struct drm_vc4_wait_qpu)
#define DRM_IOCTL_VC4_QPU_BO_ADDR         DRM_IOWR(DRM_COMMAND_BASE + DRM_VC4_QPU_BO_ADDR, struct drm_vc4_qpu_bo_addr)

/* DRM_IOCTL_VC4_GET_PARAM parameter: non-zero if the ioctls above are supported */
#define DRM_VC4_PARAM_SUPPORTS_QPU_COMPUTE        0x1000

/* The depth of the hardware user program request queue */
#define VC4_MAX_QPU_PROGRAMS                      16

/**
 * struct drm_vc4_qpu_program - One QPU user program of a compute job
 * @code: GPU (bus) address of the program's code
 * @uniforms: GPU (bus) address of the program's UNIFORMs
 *
 * Both addresses must lie within one of the job's BOs.
 */
struct drm_vc4_qpu_program {
	__u32 code;
	__u32 uniforms;
};

/**
 * struct drm_vc4_submit_qpu - ioctl argument for queueing a compute job
 *
 * The job runs when all previously submitted jobs (bin/render and compute) finished, and runs alone on V3D. Jobs
 * submitted afterwards wait for it.
 */
struct drm_vc4_submit_qpu {
	/* User pointer to an array of bo_handle_count __u32 GEM handles of all BOs the programs access. */
	__u64 bo_handles;
	/* User pointer to an array of program_count struct drm_vc4_qpu_program, one per QPU. */
	__u64 programs;
	__u32 bo_handle_count;
	/* 1 - VC4_MAX_QPU_PROGRAMS */
	__u32 program_count;
	/* Maximum execution time, 0 for the default (30 s). At most 10 minutes. */
	__u32 timeout_ms;
	/* Must be 0 */
	__u32 flags;
	/* Optional syncobj to wait for before queueing the job, 0 for none */
	__u32 in_sync;
	/* Optional syncobj to replace with the job's fence, 0 for none */
	__u32 out_sync;
	/* Returned: the seqno of the job, to be passed to DRM_IOCTL_VC4_WAIT_QPU */
	__u64 seqno;
};

/**
 * struct drm_vc4_wait_qpu - ioctl argument for waiting for a compute job
 * @seqno: the seqno returned by DRM_IOCTL_VC4_SUBMIT_QPU
 * @timeout_ns: the time to wait, 0 to not block. Like for DRM_IOCTL_VC4_WAIT_SEQNO, it is updated if the wait is
 *	interrupted, so the ioctl can be restarted.
 * @status: returned once the job finished: 0 on success, -ETIMEDOUT if the job timed out (and V3D was reset), -EIO
 *	if the job timed out and the QPUs could not be stopped (compute is disabled until reboot).
 *
 * The status is tracked for the most recent failed job only.
 */
struct drm_vc4_wait_qpu {
	__u64 seqno;
	__u64 timeout_ns;
	__s32 status;
	__u32 pad;
};

/**
 * struct drm_vc4_qpu_bo_addr - ioctl argument for getting the GPU address of a BO
 * @handle: GEM handle
 * @addr: returned GPU (bus) address of the BO, as accessed by the QPUs
 */
struct drm_vc4_qpu_bo_addr {
	__u32 handle;
	__u32 addr;
};

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_VC4_COMPUTE_DRM_H_ */
