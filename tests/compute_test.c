// SPDX-License-Identifier: MIT
/*
 * Tests the vc4 compute ioctls (vc4_compute_drm.h) with hand-assembled QPU programs.
 *
 * Usage: sudo ./compute_test [--no-timeout] [/dev/dri/renderD128]
 *
 * WARNING: Test 3 runs a program that never ends, to test the timeout and GPU reset. If the reset does not work, the
 * QPUs keep spinning until reboot, compute is disabled ("wedged"), and GL rendering hangs. Run it without a desktop
 * first. --no-timeout skips tests 3 and 4.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/vc4_drm.h>

#include "../vc4/vc4_compute_drm.h"

/* QPU instructions, as emitted by VC4C (low word first) */
#define QPU_NOP 0x009e7000, 0x100009e7
#define QPU_THREAD_END 0x009e7000, 0x300009e7
/* brr (always) to itself: the offset is relative to PC + 4 instructions */
#define QPU_BRANCH_TO_SELF 0xffffffe0, 0xf0f809e7

static const uint32_t PROGRAM_END[] = {QPU_THREAD_END, QPU_NOP, QPU_NOP};
static const uint32_t PROGRAM_SPIN[] = {QPU_BRANCH_TO_SELF, QPU_NOP, QPU_NOP, QPU_NOP};

#define OFFSET_END 0
#define OFFSET_SPIN 256
#define OFFSET_UNIFORMS 1024
#define BO_SIZE 4096

static int fd;
static uint32_t bo_handle, bo_addr;
static int failures;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void check(int condition, const char* what)
{
    printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
    if(!condition)
        failures++;
}

/* Submits a job running the given program on num_qpus QPUs and waits for it. Returns the ioctl error (negative
 * errno) or the job status. */
static int run(uint32_t code_offset, unsigned num_qpus, uint32_t timeout_ms, double* elapsed_ms)
{
    struct drm_vc4_qpu_program programs[VC4_MAX_QPU_PROGRAMS];
    for(unsigned i = 0; i < num_qpus; i++)
    {
        programs[i].code = bo_addr + code_offset;
        programs[i].uniforms = bo_addr + OFFSET_UNIFORMS;
    }
    struct drm_vc4_submit_qpu submit = {
        .bo_handles = (uintptr_t) &bo_handle,
        .programs = (uintptr_t) programs,
        .bo_handle_count = 1,
        .program_count = num_qpus,
        .timeout_ms = timeout_ms,
    };
    double start = now_ms();
    if(ioctl(fd, DRM_IOCTL_VC4_SUBMIT_QPU, &submit))
        return -errno;
    struct drm_vc4_wait_qpu wait = {.seqno = submit.seqno, .timeout_ns = ~0ull};
    int ret;
    do
        ret = ioctl(fd, DRM_IOCTL_VC4_WAIT_QPU, &wait);
    while(ret && errno == EINTR);
    if(elapsed_ms)
        *elapsed_ms = now_ms() - start;
    if(ret)
        return -errno;
    return wait.status;
}

int main(int argc, char** argv)
{
    const char* path = "/dev/dri/renderD128";
    int skip_timeout = 0;
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "--no-timeout") == 0)
            skip_timeout = 1;
        else
            path = argv[i];
    }
    fd = open(path, O_RDWR);
    if(fd < 0)
    {
        perror(path);
        return 2;
    }

    char name[16] = {0};
    struct drm_version version = {.name = name, .name_len = sizeof(name) - 1};
    if(ioctl(fd, DRM_IOCTL_VERSION, &version) || strcmp(name, "vc4") != 0)
    {
        printf("%s is not a vc4 device (%s)\n", path, name);
        return 2;
    }

    struct drm_vc4_get_param param = {.param = DRM_VC4_PARAM_SUPPORTS_QPU_COMPUTE};
    if(ioctl(fd, DRM_IOCTL_VC4_GET_PARAM, &param) || !param.value)
    {
        printf("The loaded vc4 module does not support compute jobs\n");
        return 2;
    }

    struct drm_vc4_create_bo create = {.size = BO_SIZE};
    if(ioctl(fd, DRM_IOCTL_VC4_CREATE_BO, &create))
    {
        perror("CREATE_BO");
        return 2;
    }
    bo_handle = create.handle;
    struct drm_vc4_mmap_bo map = {.handle = bo_handle};
    if(ioctl(fd, DRM_IOCTL_VC4_MMAP_BO, &map))
    {
        perror("MMAP_BO");
        return 2;
    }
    uint8_t* ptr = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if(ptr == MAP_FAILED)
    {
        perror("mmap");
        return 2;
    }
    memset(ptr, 0, BO_SIZE);
    memcpy(ptr + OFFSET_END, PROGRAM_END, sizeof(PROGRAM_END));
    memcpy(ptr + OFFSET_SPIN, PROGRAM_SPIN, sizeof(PROGRAM_SPIN));

    struct drm_vc4_qpu_bo_addr addr = {.handle = bo_handle};
    int ret = ioctl(fd, DRM_IOCTL_VC4_QPU_BO_ADDR, &addr);
    printf("QPU_BO_ADDR: %s, address 0x%08x\n", ret ? strerror(errno) : "ok", addr.addr);
    if(ret)
        return errno == EPERM ? 3 : 2;
    bo_addr = addr.addr;

    printf("Test 1: programs that end immediately\n");
    double elapsed = 0, total = 0;
    check(run(OFFSET_END, 1, 0, &elapsed) == 0, "1 QPU");
    printf("         %.3f ms\n", elapsed);
    check(run(OFFSET_END, 12, 0, &elapsed) == 0, "12 QPUs");
    printf("         %.3f ms\n", elapsed);
    int ok = 1;
    for(int i = 0; i < 100; i++)
    {
        ok &= run(OFFSET_END, 12, 0, &elapsed) == 0;
        total += elapsed;
    }
    check(ok, "100 jobs of 12 QPUs");
    printf("         %.3f ms per job (submit + wait)\n", total / 100);

    printf("Test 2: invalid submissions\n");
    check(run(BO_SIZE + 64, 1, 0, NULL) == -EINVAL, "code address outside of the BOs: -EINVAL");
    check(run(OFFSET_END + 4, 1, 0, NULL) == -EINVAL, "misaligned code address: -EINVAL");
    check(run(OFFSET_END, VC4_MAX_QPU_PROGRAMS + 1, 0, NULL) == -EINVAL, "too many programs: -EINVAL");

    if(skip_timeout)
    {
        printf("Tests 3 and 4 (timeout and GPU reset) skipped\n");
        printf("%s\n", failures ? "FAILED" : "PASSED");
        return failures ? 1 : 0;
    }

    printf("Test 3: program that never ends, 500 ms timeout\n");
    ret = run(OFFSET_SPIN, 12, 500, &elapsed);
    printf("         status %d (%s) after %.0f ms\n", ret, strerror(-ret), elapsed);
    check(ret == -ETIMEDOUT, "times out with -ETIMEDOUT (GPU reset worked)");
    if(ret == -EIO)
        printf("         The QPUs could not be stopped, compute is disabled until reboot\n");

    printf("Test 4: next job after the timeout\n");
    ret = run(OFFSET_END, 12, 0, &elapsed);
    check(ret == 0, "succeeds");
    printf("         status %d after %.3f ms\n", ret, elapsed);

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
