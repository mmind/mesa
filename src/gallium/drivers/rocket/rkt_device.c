/*
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * SPDX-License-Identifier: MIT
 */

#include "rkt_device.h"
#include "rkt_ml.h"

#include "util/os_mman.h"
#include "util/ralloc.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "drm-uapi/rknpu_ioctl.h"
#include <xf86drm.h>

static void
rkt_destroy_screen(struct pipe_screen *pscreen)
{
   struct rkt_screen *screen = rkt_screen(pscreen);

   if (screen->ro)
      screen->ro->destroy(screen->ro);

   //rkt_close_device(&screen->dev);
   //disk_cache_destroy(screen->disk_cache);
   ralloc_free(screen);
}

static void
rkt_destroy_context(struct pipe_context *pctx)
{
   struct rkt_context *ctx = rkt_context(pctx);

   ralloc_free(ctx);
}

static void *
rkt_buffer_map(struct pipe_context *pctx, struct pipe_resource *prsc,
                 unsigned level,
                 unsigned usage,
                 const struct pipe_box *box,
                 struct pipe_transfer **out_transfer)
{
   struct rkt_screen *screen = rkt_screen(pctx->screen);
   struct rkt_context *ctx = rkt_context(pctx);
   struct rkt_resource *rsc = rkt_resource(prsc);
   struct rknpu_mem_map arg = {0,};
   int ret;

   assert(level == 0);
   assert(prsc->target == PIPE_BUFFER);
   assert(box->y == 0);
   assert(box->z == 0);
   assert(box->height == 1);
   assert(box->depth == 1);

   arg.handle = rsc->handle;

   ret = drmIoctl(screen->fd, DRM_IOCTL_RKNPU_MEM_MAP, &arg);
   assert(ret >= 0);

   uint8_t *map = mmap(NULL, prsc->width0, PROT_READ | PROT_WRITE, MAP_SHARED, screen->fd, arg.offset);
   assert(map != MAP_FAILED);

   struct pipe_transfer *transfer = rzalloc(NULL, struct pipe_transfer);
   transfer->level = level;
   transfer->usage = usage;
   transfer->box = *box;

   pipe_resource_reference(&transfer->resource, prsc);

   struct rknpu_mem_sync sync = {
      .obj_addr = rsc->obj_addr,
      .offset = 0,
      .size = rsc->bo_size,
      .flags = RKNPU_MEM_SYNC_FROM_DEVICE,
   };
   ret = drmIoctl(screen->fd, DRM_IOCTL_RKNPU_MEM_SYNC, &sync);
   assert(ret == 0);

   *out_transfer = transfer;

   return map + box->x;
}

static void
rkt_buffer_unmap(struct pipe_context *pctx, struct pipe_transfer *transfer)
{
   struct rkt_screen *screen = rkt_screen(pctx->screen);
   struct rkt_resource *rsrc = rkt_resource(transfer->resource);
   int ret;
   struct rknpu_mem_sync arg = {
      .obj_addr = rsrc->obj_addr,
      .offset = 0,
      .size = rsrc->bo_size,
      .flags = RKNPU_MEM_SYNC_TO_DEVICE,
   };
   ret = drmIoctl(screen->fd, DRM_IOCTL_RKNPU_MEM_SYNC, &arg);
   assert(ret == 0);

   pipe_resource_reference(&transfer->resource, NULL);
   ralloc_free(transfer);
}

static struct pipe_context *
rkt_create_context(struct pipe_screen *screen, void *priv, unsigned flags)
{
   struct rkt_context *ctx = rzalloc(NULL, struct rkt_context);
   struct pipe_context *pctx = &ctx->base;

   if (!ctx)
      return NULL;

   pctx->screen = screen;
   pctx->priv = priv;

   pctx->destroy = rkt_destroy_context;

   pctx->buffer_map = rkt_buffer_map;
   pctx->buffer_unmap = rkt_buffer_unmap;
   pctx->resource_copy_region = util_resource_copy_region;
   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->clear_buffer = u_default_clear_buffer;

   pctx->ml_subgraph_create = rkt_ml_subgraph_create;
   pctx->ml_subgraph_invoke = rkt_ml_subgraph_invoke;
   pctx->ml_subgraph_read_output = rkt_ml_subgraph_read_outputs;
   pctx->ml_subgraph_destroy = rkt_ml_subgraph_destroy;

   /*
   pctx->flush = rkt_flush;
   pctx->clear = rkt_clear;
   pctx->flush_resource = rkt_flush_resource;


   pctx->set_debug_callback = u_default_set_debug_callback;
   pctx->invalidate_resource = rkt_invalidate_resource;
   pctx->memory_barrier = rkt_memory_barrier;

   pctx->create_fence_fd = rkt_create_fence_fd;
   pctx->fence_server_sync = rkt_fence_server_sync;

   pctx->get_device_reset_status = asahi_get_device_reset_status;
   */

   return pctx;
}

static struct pipe_resource *
rkt_resource_create(struct pipe_screen *pscreen,
                      const struct pipe_resource *templat)
{
   struct rkt_screen *screen = rkt_screen(pscreen);
   struct rkt_resource *rsc;
   struct rknpu_mem_create arg = {0};
   int ret;

   assert(templat->target == PIPE_BUFFER);
   assert(templat->height0 == 1);
   assert(templat->depth0 == 1);
   assert(templat->array_size == 1);

   rsc = rzalloc(NULL, struct rkt_resource);
   if (!rsc)
      return NULL;

   rsc->base = *templat;
   rsc->base.screen = pscreen;
   rsc->base.nr_samples = templat->nr_samples;
   pipe_reference_init(&rsc->base.reference, 1);

   rsc->bo_size = templat->width0;

   arg.size = templat->width0;
   arg.flags = RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_CACHEABLE | RKNPU_MEM_KERNEL_MAPPING | RKNPU_MEM_ZEROING;

   ret = drmIoctl(screen->fd, DRM_IOCTL_RKNPU_MEM_CREATE, &arg);
   assert(ret >= 0);

   rsc->handle = arg.handle;
   rsc->phys_addr = arg.dma_addr;
   rsc->obj_addr = arg.obj_addr;

#if 0
   if (DBG_ENABLED(ROCKET_DBG_ZERO)) {
      void *map = rkt_bo_map(rsc->bo);
      rkt_bo_cpu_prep(rsc->bo, DRM_rkt_PREP_WRITE);
      memset(map, 0, size);
      rkt_bo_cpu_fini(rsc->bo);
   }
#endif

   return &rsc->base;

free_rsc:
   ralloc_free(rsc);
   return NULL;
}

static void
rkt_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct rkt_screen *screen = rkt_screen(pscreen);
   struct rkt_resource *rsc = rkt_resource(prsc);
   struct rknpu_mem_destroy arg = {0};
   int ret;

   arg.handle = rsc->handle;

   ret = drmIoctl(screen->fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &arg);
   assert(ret >= 0);

   ralloc_free(rsc);
}

static int
rkt_screen_get_fd(struct pipe_screen *pscreen)
{
   return rkt_screen(pscreen)->fd;
}

struct pipe_screen *
rkt_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro)
{
   struct rkt_screen *rkt_screen;
   struct pipe_screen *screen;
   int ret;

   rkt_screen = rzalloc(NULL, struct rkt_screen);
   if (!rkt_screen)
      return NULL;

   screen = &rkt_screen->pscreen;

   /* Set debug before opening */
   //rkt_screen->dev.debug =
   //   debug_get_flags_option("RKT_MESA_DEBUG", rkt_debug_options, 0);

   rkt_screen->fd = fd;

   screen->get_screen_fd = rkt_screen_get_fd;
   screen->destroy = rkt_destroy_screen;
   screen->context_create = rkt_create_context;
   screen->resource_create = rkt_resource_create;
   screen->resource_destroy = rkt_resource_destroy;

   struct rknpu_action action = {
      .flags = RKNPU_SET_PROC_NICE,
      .value = 0xffffffed,
   };

   ret = drmIoctl(fd, DRM_IOCTL_RKNPU_ACTION, &action);
   assert(ret >= 0);

   return screen;
}