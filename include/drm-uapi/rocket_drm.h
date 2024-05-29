/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Tomeu Vizoso
 */
#ifndef _ROCKET_DRM_H_
#define _ROCKET_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_ROCKET_CREATE_BO			0x00
#define DRM_ROCKET_PREP_BO			0x01
#define DRM_ROCKET_FINI_BO			0x02
#define DRM_ROCKET_SUBMIT			0x03

#define DRM_IOCTL_ROCKET_CREATE_BO		DRM_IOWR(DRM_COMMAND_BASE + DRM_ROCKET_CREATE_BO, struct drm_rocket_create_bo)
#define DRM_IOCTL_ROCKET_PREP_BO		DRM_IOW(DRM_COMMAND_BASE + DRM_ROCKET_PREP_BO, struct drm_rocket_prep_bo)
#define DRM_IOCTL_ROCKET_FINI_BO		DRM_IOW(DRM_COMMAND_BASE + DRM_ROCKET_FINI_BO, struct drm_rocket_fini_bo)
#define DRM_IOCTL_ROCKET_SUBMIT			DRM_IOW(DRM_COMMAND_BASE + DRM_ROCKET_SUBMIT, struct drm_rocket_submit)

/* Please note that modifications to all structs defined here are
 * subject to backwards-compatibility constraints:
 *  1) Do not use pointers, use __u64 instead for 32 bit / 64 bit
 *     user/kernel compatibility
 *  2) Keep fields aligned to their size
 *  3) Because of how drm_ioctl() works, we can add new fields at
 *     the end of an ioctl if some care is taken: drm_ioctl() will
 *     zero out the new fields at the tail of the ioctl, so a zero
 *     value should have a backwards compatible meaning.  And for
 *     output params, userspace won't see the newly added output
 *     fields.. so that has to be somehow ok.
 */

/* timeouts are specified in clock-monotonic absolute times (to simplify
 * restarting interrupted ioctls).  The following struct is logically the
 * same as 'struct timespec' but 32/64b ABI safe.
 */
struct drm_rocket_timespec {
	__s64 tv_sec;          /* seconds */
	__s64 tv_nsec;         /* nanoseconds */
};

/**
 * struct drm_rocket_create_bo - ioctl argument for creating Rocket BOs.
 *
 * The flags argument is a bit mask of ROCKET_BO_* flags.
 */
struct drm_rocket_create_bo {
	__u32 size;
	__u32 flags;

	/** Returned GEM handle for the BO. */
	__u32 handle;

	/** Pad, must be zero-filled. */
	__u32 pad;

	/**
	 * Returned DMA address for the BO in the NPU address space.  This address
	 * is private to the DRM fd and is valid for the lifetime of the GEM
	 * handle.
	 *
	 * This address value will always be nonzero, since various HW
	 * units treat 0 specially.
	 */
	__u64 dma_address;

	/** Offset into the drm node to use for subsequent mmap call. */
	__u64 offset;
};

#define ROCKET_PREP_READ        0x01
#define ROCKET_PREP_WRITE       0x02
#define ROCKET_PREP_NOSYNC      0x04

struct drm_rocket_prep_bo {
	__u32 handle;		/* in */
	__u32 op;		/* in, mask of ROCKET_PREP_x */
	__s64 timeout_ns;	/* in */
};

struct drm_rocket_fini_bo {
	__u32 handle;		/* in */
	__u32 flags;		/* in, placeholder for now, no defined values */
};

/**
 * struct drm_rocket_submit - ioctl argument for submitting commands to the NPU.
 *
 * This asks the kernel to have the NPU execute a register command list.
 */
struct drm_rocket_submit {

       /** DMA address to NPU mapping of register command buffer */
       __u64 regcmd;

       /** An optional array of sync objects to wait on before starting this job. */
       __u64 in_syncs;

       /** Number of sync objects to wait on before starting this job. */
       __u32 in_sync_count;

       /** An optional sync object to place the completion fence in. */
       __u32 out_sync;

       /** Pointer to a u32 array of the BOs that are referenced by the job. */
       __u64 bo_handles;

       /** Number of BO handles passed in (size is that times 4). */
       __u32 bo_handle_count;
};

#if defined(__cplusplus)
}
#endif

#endif /* _ROCKET_DRM_H_ */
