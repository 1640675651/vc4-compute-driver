// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compute jobs: QPU user programs (e.g. OpenCL kernels compiled by VC4C) run as jobs scheduled by the vc4 driver.
 * See VC4CL/DRM-DESIGN.md for the design.
 *
 * A compute job is queued in the bin job list like a bin/render job, but runs alone on V3D: It is only started when
 * it is at the head of the bin job list (so nothing is binning) and no render job is left. Jobs queued after it wait
 * until it finished.
 *
 * Since the VideoCore IV has no MMU, QPU programs can access all of physical memory, so all ioctls in here require
 * CAP_SYS_RAWIO, or membership in the group set by the compute_gid module parameter.
 */

#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dma-fence-array.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_syncobj.h>

#include "vc4_drv.h"
#include "vc4_regs.h"

/* VPM memory reserved for user programs while a compute job runs, in
 * multiples of 256 bytes. 16 * 256 = 4 KB is the most VC4C uses.
 */
#define VC4_COMPUTE_VPM_RESERVATION	16

/* V3D_SRQCS fields */
#define V3D_SRQCS_QPURQL_MASK		GENMASK(5, 0)	/* request queue length */
#define V3D_SRQCS_QPURQERR		BIT(7)		/* queue error, write 1 to clear */
#define V3D_SRQCS_QPURQCM_MASK		GENMASK(15, 8)	/* requests made, write 1 to bit 8 to clear */
#define V3D_SRQCS_QPURQCM_CLEAR		BIT(8)
#define V3D_SRQCS_QPURQCC_MASK		GENMASK(23, 16)	/* requests completed, write 1 to bit 16 to clear */
#define V3D_SRQCS_QPURQCC_SHIFT		16
#define V3D_SRQCS_QPURQCC_CLEAR		BIT(16)

/* Members of this group (a GID of the initial user namespace) may use the
 * compute ioctls without CAP_SYS_RAWIO. Since QPU programs can access all of
 * physical memory, this effectively gives them root rights, so it is disabled
 * (-1) by default.
 */
static int compute_gid = -1;
module_param(compute_gid, int, 0644);
MODULE_PARM_DESC(compute_gid,
		 "Group allowed to run QPU compute jobs without CAP_SYS_RAWIO, effectively giving it root rights (default: -1, none)");

#define VC4_COMPUTE_DEFAULT_TIMEOUT_MS	30000
#define VC4_COMPUTE_MAX_TIMEOUT_MS	(10 * 60 * 1000)

/* Poll often at first, since most jobs are short, then back off to not
 * waste CPU time on long jobs.
 */
#define VC4_COMPUTE_POLL_FAST_NS	(100 * NSEC_PER_USEC)
#define VC4_COMPUTE_POLL_SLOW_NS	NSEC_PER_MSEC
#define VC4_COMPUTE_POLL_FAST_PERIOD_NS	(10 * NSEC_PER_MSEC)

static unsigned int vc4_compute_completed_count(struct vc4_dev *vc4)
{
	return (V3D_READ(V3D_SRQCS) & V3D_SRQCS_QPURQCC_MASK) >>
	       V3D_SRQCS_QPURQCC_SHIFT;
}

/* Completes the running compute job @exec, which is at the head of the bin
 * job list, and starts the next job.
 *
 * With @status -EIO, the QPU programs might still be running, so the job is
 * never freed, which keeps its BOs (and their memory) alive.
 *
 * Called with job_lock held.
 */
static void vc4_compute_finish(struct drm_device *dev,
			       struct vc4_exec_info *exec, int status)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	/* Give the VPM back to the vertex pipeline, as vc4_v3d_init_hw() */
	V3D_WRITE(V3D_VPMBASE, 0);

	if (status) {
		vc4->compute.failed_seqno = exec->seqno;
		vc4->compute.failed_status = status;
		if (exec->fence)
			dma_fence_set_error(exec->fence, status);
	}

	/* Everything queued before this job finished before it was started,
	 * so this keeps finished_seqno in order.
	 */
	vc4->finished_seqno++;
	list_move_tail(&exec->head, status == -EIO ?
		       &vc4->compute.wedged_list : &vc4->job_done_list);

	if (exec->fence) {
		dma_fence_signal_locked(exec->fence);
		dma_fence_put(exec->fence);
		exec->fence = NULL;
	}

	wake_up_all(&vc4->job_wait_queue);
	schedule_work(&vc4->job_done_work);

	vc4_submit_next_bin_job(dev);
	/* The hangcheck stops while only a compute job is queued */
	if (vc4_first_bin_job(vc4) || vc4_first_render_job(vc4))
		vc4_queue_hangcheck(dev);
}

static enum hrtimer_restart vc4_compute_poll(struct hrtimer *timer)
{
	struct vc4_dev *vc4 = container_of(timer, struct vc4_dev,
					   compute.timer);
	struct drm_device *dev = &vc4->base;
	enum hrtimer_restart ret = HRTIMER_NORESTART;
	struct vc4_exec_info *exec;
	unsigned long irqflags;
	s64 elapsed_ns;

	spin_lock_irqsave(&vc4->job_lock, irqflags);

	exec = vc4_first_bin_job(vc4);
	if (!exec || !exec->is_compute || !exec->compute_started ||
	    exec->compute_timed_out)
		goto out;

	if (vc4_compute_completed_count(vc4) >= exec->qpu_program_count) {
		vc4_compute_finish(dev, exec, 0);
		goto out;
	}

	elapsed_ns = ktime_to_ns(ktime_sub(ktime_get(),
					   exec->compute_start_time));
	if (elapsed_ns > (s64)exec->compute_timeout_ms * NSEC_PER_MSEC) {
		exec->compute_timed_out = true;
		DRM_ERROR("Compute job %llu timed out after %u ms, resetting GPU\n",
			  exec->seqno, exec->compute_timeout_ms);
		/* vc4_reset() cancels the job, see vc4_compute_cancel() */
		schedule_work(&vc4->hangcheck.reset_work);
		goto out;
	}

	hrtimer_forward_now(timer,
			    ns_to_ktime(elapsed_ns < VC4_COMPUTE_POLL_FAST_PERIOD_NS ?
					VC4_COMPUTE_POLL_FAST_NS :
					VC4_COMPUTE_POLL_SLOW_NS));
	ret = HRTIMER_RESTART;

out:
	spin_unlock_irqrestore(&vc4->job_lock, irqflags);
	return ret;
}

/* Hands the programs of the compute job @exec at the head of the bin job list
 * to the hardware, unless a render job is still running. In that case,
 * vc4_irq_finish_render_job() calls this again once rendering finished.
 *
 * Called with job_lock held.
 */
void vc4_compute_start(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	unsigned int i;

	lockdep_assert_held(&vc4->job_lock);

	if (exec->compute_started || vc4_first_render_job(vc4))
		return;

	exec->compute_started = true;

	/* The CPU just wrote the code and UNIFORMs */
	vc4_flush_caches(dev);

	V3D_WRITE(V3D_VPMBASE, VC4_COMPUTE_VPM_RESERVATION);

	V3D_WRITE(V3D_SRQCS, V3D_SRQCS_QPURQERR | V3D_SRQCS_QPURQCM_CLEAR |
			     V3D_SRQCS_QPURQCC_CLEAR);

	/* Writing the code address queues the request */
	for (i = 0; i < exec->qpu_program_count; i++) {
		V3D_WRITE(V3D_SRQUA, exec->qpu_programs[i].uniforms);
		V3D_WRITE(V3D_SRQPC, exec->qpu_programs[i].code);
	}

	exec->compute_start_time = ktime_get();
	hrtimer_start(&vc4->compute.timer, ns_to_ktime(VC4_COMPUTE_POLL_FAST_NS),
		      HRTIMER_MODE_REL);
}

/* Called by vc4_irq_reset() (with job_lock held) for a running compute job at
 * the head of the bin job list, after vc4_reset() tried to power-cycle V3D.
 */
void vc4_compute_cancel(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	u32 srqcs;

	lockdep_assert_held(&vc4->job_lock);

	/* Does not wait for a running callback, which would deadlock on
	 * job_lock. The callback will not find the job anymore.
	 */
	hrtimer_try_to_cancel(&vc4->compute.timer);

	srqcs = V3D_READ(V3D_SRQCS);
	if (!(srqcs & (V3D_SRQCS_QPURQL_MASK | V3D_SRQCS_QPURQCM_MASK |
		       V3D_SRQCS_QPURQCC_MASK))) {
		/* Only a real power cycle resets all user program counters */
		vc4_compute_finish(dev, exec, -ETIMEDOUT);
		return;
	}
	if (vc4_compute_completed_count(vc4) >= exec->qpu_program_count) {
		/* Finished just before the reset */
		vc4_compute_finish(dev, exec, 0);
		return;
	}

	DRM_ERROR("QPU programs of compute job %llu still running after GPU reset (SRQCS 0x%08x), disabling compute jobs until reboot\n",
		  exec->seqno, srqcs);
	vc4->compute.wedged = true;
	vc4_compute_finish(dev, exec, -EIO);
}

void vc4_compute_init(struct vc4_dev *vc4)
{
	hrtimer_init(&vc4->compute.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vc4->compute.timer.function = vc4_compute_poll;
	INIT_LIST_HEAD(&vc4->compute.wedged_list);
}

void vc4_compute_destroy(struct vc4_dev *vc4)
{
	hrtimer_cancel(&vc4->compute.timer);
	/* Jobs on the wedged list are leaked on purpose. */
}

static bool vc4_compute_permitted(void)
{
	int gid = READ_ONCE(compute_gid);

	/* Checked first, since capable() logs denials */
	if (gid >= 0 && in_group_p(make_kgid(&init_user_ns, gid)))
		return true;

	return capable(CAP_SYS_RAWIO);
}

static int vc4_compute_check_access(struct vc4_dev *vc4)
{
	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	if (!vc4->v3d)
		return -ENODEV;

	/* QPU programs can access all of physical memory */
	if (!vc4_compute_permitted())
		return -EPERM;

	return 0;
}

/* Like vc4_cl_lookup_bos(), sets up exec->bo[] with references and use counts
 * of all BOs the job's programs access.
 */
static int vc4_compute_lookup_bos(struct drm_file *file_priv,
				  struct vc4_exec_info *exec,
				  u64 handles, u32 count)
{
	int ret;
	int i;

	exec->bo_count = count;

	ret = drm_gem_objects_lookup(file_priv, u64_to_user_ptr(handles),
				     count, &exec->bo);
	if (ret)
		goto fail_put_bo;

	for (i = 0; i < exec->bo_count; i++) {
		ret = vc4_bo_inc_usecnt(to_vc4_bo(exec->bo[i]));
		if (ret)
			goto fail_dec_usecnt;
	}

	return 0;

fail_dec_usecnt:
	for (i--; i >= 0; i--)
		vc4_bo_dec_usecnt(to_vc4_bo(exec->bo[i]));

fail_put_bo:
	for (i = 0; i < exec->bo_count && exec->bo && exec->bo[i]; i++)
		drm_gem_object_put(exec->bo[i]);

	kvfree(exec->bo);
	exec->bo = NULL;
	return ret;
}

static bool vc4_compute_address_in_bos(struct vc4_exec_info *exec, u32 addr)
{
	unsigned int i;

	for (i = 0; i < exec->bo_count; i++) {
		struct drm_gem_dma_object *obj = &to_vc4_bo(exec->bo[i])->base;

		if (addr >= obj->dma_addr &&
		    addr - obj->dma_addr < obj->base.size)
			return true;
	}

	return false;
}

int vc4_submit_qpu_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_vc4_submit_qpu *args = data;
	struct drm_syncobj *out_sync = NULL;
	struct ww_acquire_ctx acquire_ctx;
	struct vc4_exec_info *exec;
	struct dma_fence *in_fence;
	unsigned int i;
	int ret;

	ret = vc4_compute_check_access(vc4);
	if (ret)
		return ret;

	if (READ_ONCE(vc4->compute.wedged))
		return -EIO;

	if (args->flags || !args->bo_handle_count ||
	    !args->program_count ||
	    args->program_count > VC4_MAX_QPU_PROGRAMS ||
	    args->timeout_ms > VC4_COMPUTE_MAX_TIMEOUT_MS)
		return -EINVAL;

	exec = kzalloc(sizeof(*exec), GFP_KERNEL);
	if (!exec)
		return -ENOMEM;
	exec->dev = vc4;
	exec->is_compute = true;
	exec->compute_timeout_ms = args->timeout_ms ?:
				   VC4_COMPUTE_DEFAULT_TIMEOUT_MS;
	INIT_LIST_HEAD(&exec->unref_list);

	ret = vc4_v3d_pm_get(vc4);
	if (ret) {
		kfree(exec);
		return ret;
	}

	exec->qpu_programs = kmalloc_array(args->program_count,
					   sizeof(*exec->qpu_programs),
					   GFP_KERNEL);
	if (!exec->qpu_programs) {
		ret = -ENOMEM;
		goto fail;
	}
	if (copy_from_user(exec->qpu_programs, u64_to_user_ptr(args->programs),
			   args->program_count * sizeof(*exec->qpu_programs))) {
		ret = -EFAULT;
		goto fail;
	}
	exec->qpu_program_count = args->program_count;

	ret = vc4_compute_lookup_bos(file_priv, exec, args->bo_handles,
				     args->bo_handle_count);
	if (ret)
		goto fail;

	/* Not a security check (the programs can access anything anyway), but
	 * catches broken submissions before they hang the GPU.
	 */
	for (i = 0; i < exec->qpu_program_count; i++) {
		struct drm_vc4_qpu_program *program = &exec->qpu_programs[i];

		if ((program->code & 7) || (program->uniforms & 3) ||
		    !vc4_compute_address_in_bos(exec, program->code) ||
		    !vc4_compute_address_in_bos(exec, program->uniforms)) {
			DRM_DEBUG("Compute program %u has invalid code 0x%08x or UNIFORMs 0x%08x address\n",
				  i, program->code, program->uniforms);
			ret = -EINVAL;
			goto fail;
		}
	}

	if (args->in_sync) {
		ret = drm_syncobj_find_fence(file_priv, args->in_sync,
					     0, 0, &in_fence);
		if (ret)
			goto fail;

		/* Fences of our own context are from earlier jobs, which run
		 * before this one anyway.
		 */
		if (!dma_fence_match_context(in_fence,
					     vc4->dma_fence_context)) {
			ret = dma_fence_wait(in_fence, true);
			if (ret) {
				dma_fence_put(in_fence);
				goto fail;
			}
		}

		dma_fence_put(in_fence);
	}

	/* Looked up before taking the reservation locks, so failing does not
	 * leave them locked.
	 */
	if (args->out_sync) {
		out_sync = drm_syncobj_find(file_priv, args->out_sync);
		if (!out_sync) {
			ret = -EINVAL;
			goto fail;
		}
	}

	ret = vc4_lock_bo_reservations(dev, exec, &acquire_ctx);
	if (ret)
		goto fail_put_sync;

	ret = vc4_queue_submit(dev, exec, &acquire_ctx, out_sync, &args->seqno);

fail_put_sync:
	if (out_sync)
		drm_syncobj_put(out_sync);
	if (ret)
		goto fail;

	return 0;

fail:
	vc4_complete_exec(dev, exec);
	return ret;
}

int vc4_wait_qpu_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_vc4_wait_qpu *args = data;
	unsigned long start = jiffies;
	unsigned long irqflags;
	int ret;

	ret = vc4_compute_check_access(vc4);
	if (ret)
		return ret;

	if (args->pad)
		return -EINVAL;

	ret = vc4_wait_for_seqno(dev, args->seqno, args->timeout_ns, true);
	if (ret) {
		/* Like vc4_wait_for_seqno_ioctl_helper(): allow restarting
		 * the ioctl with the remaining time.
		 */
		if ((ret == -EINTR || ret == -ERESTARTSYS) &&
		    args->timeout_ns != ~0ull) {
			u64 delta = jiffies_to_nsecs(jiffies - start);

			if (args->timeout_ns >= delta)
				args->timeout_ns -= delta;
		}
		return ret;
	}

	spin_lock_irqsave(&vc4->job_lock, irqflags);
	args->status = vc4->compute.failed_seqno == args->seqno ?
		       vc4->compute.failed_status : 0;
	spin_unlock_irqrestore(&vc4->job_lock, irqflags);

	return 0;
}

int vc4_qpu_bo_addr_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_vc4_qpu_bo_addr *args = data;
	struct drm_gem_object *gem_obj;
	dma_addr_t addr;
	int ret;

	ret = vc4_compute_check_access(vc4);
	if (ret)
		return ret;

	gem_obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!gem_obj)
		return -ENOENT;

	addr = to_vc4_bo(gem_obj)->base.dma_addr;
	drm_gem_object_put(gem_obj);

	/* The QPUs only have 32-bit addresses */
	if (upper_32_bits(addr))
		return -EINVAL;

	args->addr = lower_32_bits(addr);
	return 0;
}
