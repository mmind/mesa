/*
 * Copyright © 2024 Collabora Ltd.
 * Copyright © 2024 Arm Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "panvk_buffer.h"
#include "panvk_cmd_alloc.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_cmd_draw.h"
#include "panvk_cmd_fb_preload.h"
#include "panvk_cmd_meta.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_image.h"
#include "panvk_image_view.h"
#include "panvk_instance.h"
#include "panvk_priv_bo.h"
#include "panvk_shader.h"

#include "pan_desc.h"
#include "pan_earlyzs.h"
#include "pan_encoder.h"
#include "pan_format.h"
#include "pan_jc.h"
#include "pan_props.h"
#include "pan_samples.h"
#include "pan_shader.h"

#include "vk_format.h"
#include "vk_meta.h"
#include "vk_pipeline_layout.h"
#include "vk_render_pass.h"

struct panvk_draw_info {
   struct {
      uint32_t size;
      uint32_t offset;
   } index;

   struct {
      uint32_t base;
      uint32_t count;
   } vertex;

   struct {
      uint32_t base;
      uint32_t count;
   } instance;

   struct {
      struct panvk_buffer *buffer;
      uint64_t offset;
      uint32_t draw_count;
      uint32_t stride;
   } indirect;
};

static void
emit_vs_attrib(const struct vk_vertex_attribute_state *attrib_info,
               const struct vk_vertex_binding_state *buf_info,
               const struct panvk_attrib_buf *buf, uint32_t vb_desc_offset,
               struct mali_attribute_packed *desc)
{
   bool per_instance = buf_info->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE;
   enum pipe_format f = vk_format_to_pipe_format(attrib_info->format);
   unsigned buf_idx = vb_desc_offset + attrib_info->binding;

   pan_pack(desc, ATTRIBUTE, cfg) {
      cfg.offset = attrib_info->offset;
      cfg.format = GENX(panfrost_format_from_pipe_format)(f)->hw;
      cfg.table = 0;
      cfg.buffer_index = buf_idx;
      cfg.stride = buf_info->stride;
      if (!per_instance) {
         /* Per-vertex */
         cfg.attribute_type = MALI_ATTRIBUTE_TYPE_1D;
         cfg.frequency = MALI_ATTRIBUTE_FREQUENCY_VERTEX;
         cfg.offset_enable = true;
      } else if (buf_info->divisor == 1) {
         cfg.attribute_type = MALI_ATTRIBUTE_TYPE_1D;
         cfg.frequency = MALI_ATTRIBUTE_FREQUENCY_INSTANCE;
      } else if (util_is_power_of_two_or_zero(buf_info->divisor)) {
         /* Per-instance, POT divisor */
         cfg.attribute_type = MALI_ATTRIBUTE_TYPE_1D_POT_DIVISOR;
         cfg.frequency = MALI_ATTRIBUTE_FREQUENCY_INSTANCE;
         cfg.divisor_r = __builtin_ctz(buf_info->divisor);
      } else {
         /* Per-instance, NPOT divisor */
         cfg.attribute_type = MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR;
         cfg.frequency = MALI_ATTRIBUTE_FREQUENCY_INSTANCE;
         cfg.divisor_d = panfrost_compute_magic_divisor(
            buf_info->divisor, &cfg.divisor_r, &cfg.divisor_e);
      }
   }
}

static bool
vs_driver_set_is_dirty(struct panvk_cmd_buffer *cmdbuf)
{
   return dyn_gfx_state_dirty(cmdbuf, VI) ||
          dyn_gfx_state_dirty(cmdbuf, VI_BINDINGS_VALID) ||
          dyn_gfx_state_dirty(cmdbuf, VI_BINDING_STRIDES) ||
          gfx_state_dirty(cmdbuf, VB) || gfx_state_dirty(cmdbuf, VS) ||
          gfx_state_dirty(cmdbuf, DESC_STATE);
}

static VkResult
prepare_vs_driver_set(struct panvk_cmd_buffer *cmdbuf)
{
   if (!vs_driver_set_is_dirty(cmdbuf))
      return VK_SUCCESS;

   struct panvk_shader_desc_state *vs_desc_state = &cmdbuf->state.gfx.vs.desc;
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   const struct vk_vertex_input_state *vi =
      cmdbuf->vk.dynamic_graphics_state.vi;
   unsigned num_vs_attribs = util_last_bit(vi->attributes_valid);
   uint32_t vb_count = 0;

   for (unsigned i = 0; i < num_vs_attribs; i++) {
      if (vi->attributes_valid & BITFIELD_BIT(i))
         vb_count = MAX2(vi->attributes[i].binding + 1, vb_count);
   }

   uint32_t vb_offset = vs->desc_info.dyn_bufs.count + MAX_VS_ATTRIBS + 1;
   uint32_t desc_count = vb_offset + vb_count;
   const struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.gfx.desc_state;
   struct panfrost_ptr driver_set = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, desc_count * PANVK_DESCRIPTOR_SIZE, PANVK_DESCRIPTOR_SIZE);
   struct panvk_opaque_desc *descs = driver_set.cpu;

   if (!driver_set.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t i = 0; i < MAX_VS_ATTRIBS; i++) {
      if (vi->attributes_valid & BITFIELD_BIT(i)) {
         unsigned binding = vi->attributes[i].binding;

         emit_vs_attrib(&vi->attributes[i], &vi->bindings[binding],
                        &cmdbuf->state.gfx.vb.bufs[binding], vb_offset,
                        (struct mali_attribute_packed *)(&descs[i]));
      } else {
         memset(&descs[i], 0, sizeof(descs[0]));
      }
   }

   /* Dummy sampler always comes right after the vertex attribs. */
   pan_pack(&descs[MAX_VS_ATTRIBS], SAMPLER, _) {
   }

   panvk_per_arch(cmd_fill_dyn_bufs)(
      desc_state, vs,
      (struct mali_buffer_packed *)(&descs[MAX_VS_ATTRIBS + 1]));

   for (uint32_t i = 0; i < vb_count; i++) {
      const struct panvk_attrib_buf *vb = &cmdbuf->state.gfx.vb.bufs[i];

      pan_pack(&descs[vb_offset + i], BUFFER, cfg) {
         if (vi->bindings_valid & BITFIELD_BIT(i)) {
            cfg.address = vb->address;
            cfg.size = vb->size;
         } else {
            cfg.address = 0;
            cfg.size = 0;
         }
      }
   }

   vs_desc_state->driver_set.dev_addr = driver_set.gpu;
   vs_desc_state->driver_set.size = desc_count * PANVK_DESCRIPTOR_SIZE;
   gfx_state_set_dirty(cmdbuf, DESC_STATE);
   return VK_SUCCESS;
}

static VkResult
prepare_fs_driver_set(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_shader_desc_state *fs_desc_state = &cmdbuf->state.gfx.fs.desc;
   const struct panvk_shader *fs = cmdbuf->state.gfx.fs.shader;
   const struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.gfx.desc_state;
   uint32_t desc_count = fs->desc_info.dyn_bufs.count + 1;
   struct panfrost_ptr driver_set = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, desc_count * PANVK_DESCRIPTOR_SIZE, PANVK_DESCRIPTOR_SIZE);
   struct panvk_opaque_desc *descs = driver_set.cpu;

   if (desc_count && !driver_set.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Dummy sampler always comes first. */
   pan_pack(&descs[0], SAMPLER, _) {
   }

   panvk_per_arch(cmd_fill_dyn_bufs)(desc_state, fs,
                                     (struct mali_buffer_packed *)(&descs[1]));

   fs_desc_state->driver_set.dev_addr = driver_set.gpu;
   fs_desc_state->driver_set.size = desc_count * PANVK_DESCRIPTOR_SIZE;
   gfx_state_set_dirty(cmdbuf, DESC_STATE);
   return VK_SUCCESS;
}

/* This value has been selected to get
 * dEQP-VK.draw.renderpass.inverted_depth_ranges.nodepthclamp_deltazero passing.
 */
#define MIN_DEPTH_CLIP_RANGE 37.7E-06f

static void
prepare_sysvals(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_graphics_sysvals *sysvals = &cmdbuf->state.gfx.sysvals;
   struct vk_color_blend_state *cb = &cmdbuf->vk.dynamic_graphics_state.cb;
   const struct vk_rasterization_state *rs =
      &cmdbuf->vk.dynamic_graphics_state.rs;
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.render.fb.info;

   if (gfx_state_dirty(cmdbuf, RENDER_STATE)) {
      sysvals->fs.multisampled = fbinfo->nr_samples > 1;
      cmdbuf->state.gfx.push_uniforms = 0;
   }

   if (dyn_gfx_state_dirty(cmdbuf, CB_BLEND_CONSTANTS)) {
      for (unsigned i = 0; i < ARRAY_SIZE(cb->blend_constants); i++)
         sysvals->blend.constants[i] =
            CLAMP(cb->blend_constants[i], 0.0f, 1.0f);
      gfx_state_set_dirty(cmdbuf, PUSH_UNIFORMS);
   }

   if (dyn_gfx_state_dirty(cmdbuf, VP_VIEWPORTS) ||
       dyn_gfx_state_dirty(cmdbuf, RS_CULL_MODE) ||
       dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_CLAMP_ENABLE)) {
      VkViewport *viewport = &cmdbuf->vk.dynamic_graphics_state.vp.viewports[0];

      /* Upload the viewport scale. Defined as (px/2, py/2, pz) at the start of
       * section 24.5 ("Controlling the Viewport") of the Vulkan spec. At the
       * end of the section, the spec defines:
       *
       * px = width
       * py = height
       * pz = maxDepth - minDepth
       */
      sysvals->viewport.scale.x = 0.5f * viewport->width;
      sysvals->viewport.scale.y = 0.5f * viewport->height;
      sysvals->viewport.scale.z = (viewport->maxDepth - viewport->minDepth);

      /* Upload the viewport offset. Defined as (ox, oy, oz) at the start of
       * section 24.5 ("Controlling the Viewport") of the Vulkan spec. At the
       * end of the section, the spec defines:
       *
       * ox = x + width/2
       * oy = y + height/2
       * oz = minDepth
       */
      sysvals->viewport.offset.x = (0.5f * viewport->width) + viewport->x;
      sysvals->viewport.offset.y = (0.5f * viewport->height) + viewport->y;
      sysvals->viewport.offset.z = viewport->minDepth;

      /* Doing the viewport transform in the vertex shader and then depth
       * clipping with the viewport depth range gets a similar result to
       * clipping in clip-space, but loses precision when the viewport depth
       * range is very small. When minDepth == maxDepth, this completely
       * flattens the clip-space depth and results in never clipping.
       *
       * To work around this, set a lower limit on depth range when clipping is
       * enabled. This results in slightly incorrect fragment depth values, and
       * doesn't help with the precision loss, but at least clipping isn't
       * completely broken.
       */
      if (vk_rasterization_state_depth_clip_enable(rs) &&
          fabsf(sysvals->viewport.scale.z) < MIN_DEPTH_CLIP_RANGE) {
         float z_min = viewport->minDepth;
         float z_max = viewport->maxDepth;
         float z_sign = z_min <= z_max ? 1.0f : -1.0f;

         sysvals->viewport.scale.z = z_sign * MIN_DEPTH_CLIP_RANGE;

         /* Middle of the user range is
         *    z_range_center = z_min + (z_max - z_min) * 0.5f,
         * and we want to set the offset to
         *    z_offset = z_range_center - viewport.scale.z * 0.5f
         * which, when expanding, gives us
         *    z_offset = (z_max + z_min - viewport.scale.z) * 0.5f
         */
         float z_offset = (z_max + z_min - sysvals->viewport.scale.z) * 0.5f;
         /* Bump offset off-center if necessary, to not go out of range */
         sysvals->viewport.offset.z = CLAMP(z_offset, 0.0f, 1.0f);
      }
      gfx_state_set_dirty(cmdbuf, PUSH_UNIFORMS);
   }
}

static bool
has_depth_att(struct panvk_cmd_buffer *cmdbuf)
{
   return (cmdbuf->state.gfx.render.bound_attachments &
           MESA_VK_RP_ATTACHMENT_DEPTH_BIT) != 0;
}

static bool
has_stencil_att(struct panvk_cmd_buffer *cmdbuf)
{
   return (cmdbuf->state.gfx.render.bound_attachments &
           MESA_VK_RP_ATTACHMENT_STENCIL_BIT) != 0;
}

static bool
writes_depth(struct panvk_cmd_buffer *cmdbuf)
{
   const struct vk_depth_stencil_state *ds =
      &cmdbuf->vk.dynamic_graphics_state.ds;

   return has_depth_att(cmdbuf) && ds->depth.test_enable &&
          ds->depth.write_enable && ds->depth.compare_op != VK_COMPARE_OP_NEVER;
}

static bool
writes_stencil(struct panvk_cmd_buffer *cmdbuf)
{
   const struct vk_depth_stencil_state *ds =
      &cmdbuf->vk.dynamic_graphics_state.ds;

   return has_stencil_att(cmdbuf) && ds->stencil.test_enable &&
          ((ds->stencil.front.write_mask &&
            (ds->stencil.front.op.fail != VK_STENCIL_OP_KEEP ||
             ds->stencil.front.op.pass != VK_STENCIL_OP_KEEP ||
             ds->stencil.front.op.depth_fail != VK_STENCIL_OP_KEEP)) ||
           (ds->stencil.back.write_mask &&
            (ds->stencil.back.op.fail != VK_STENCIL_OP_KEEP ||
             ds->stencil.back.op.pass != VK_STENCIL_OP_KEEP ||
             ds->stencil.back.op.depth_fail != VK_STENCIL_OP_KEEP)));
}

static bool
ds_test_always_passes(struct panvk_cmd_buffer *cmdbuf)
{
   const struct vk_depth_stencil_state *ds =
      &cmdbuf->vk.dynamic_graphics_state.ds;

   if (!has_depth_att(cmdbuf))
      return true;

   if (ds->depth.test_enable && ds->depth.compare_op != VK_COMPARE_OP_ALWAYS)
      return false;

   if (ds->stencil.test_enable &&
       (ds->stencil.front.op.compare != VK_COMPARE_OP_ALWAYS ||
        ds->stencil.back.op.compare != VK_COMPARE_OP_ALWAYS))
      return false;

   return true;
}

static inline enum mali_func
translate_compare_func(VkCompareOp comp)
{
   STATIC_ASSERT(VK_COMPARE_OP_NEVER == (VkCompareOp)MALI_FUNC_NEVER);
   STATIC_ASSERT(VK_COMPARE_OP_LESS == (VkCompareOp)MALI_FUNC_LESS);
   STATIC_ASSERT(VK_COMPARE_OP_EQUAL == (VkCompareOp)MALI_FUNC_EQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_LESS_OR_EQUAL == (VkCompareOp)MALI_FUNC_LEQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_GREATER == (VkCompareOp)MALI_FUNC_GREATER);
   STATIC_ASSERT(VK_COMPARE_OP_NOT_EQUAL == (VkCompareOp)MALI_FUNC_NOT_EQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_GREATER_OR_EQUAL ==
                 (VkCompareOp)MALI_FUNC_GEQUAL);
   STATIC_ASSERT(VK_COMPARE_OP_ALWAYS == (VkCompareOp)MALI_FUNC_ALWAYS);

   return (enum mali_func)comp;
}

static enum mali_stencil_op
translate_stencil_op(VkStencilOp in)
{
   switch (in) {
   case VK_STENCIL_OP_KEEP:
      return MALI_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO:
      return MALI_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE:
      return MALI_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return MALI_STENCIL_OP_INCR_SAT;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return MALI_STENCIL_OP_DECR_SAT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return MALI_STENCIL_OP_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return MALI_STENCIL_OP_DECR_WRAP;
   case VK_STENCIL_OP_INVERT:
      return MALI_STENCIL_OP_INVERT;
   default:
      unreachable("Invalid stencil op");
   }
}

static enum mali_draw_mode
translate_prim_topology(VkPrimitiveTopology in)
{
   /* Test VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA separately, as it's not
    * part of the VkPrimitiveTopology enum.
    */
   if (in == VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA)
      return MALI_DRAW_MODE_TRIANGLES;

   switch (in) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return MALI_DRAW_MODE_POINTS;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return MALI_DRAW_MODE_LINES;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return MALI_DRAW_MODE_LINE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return MALI_DRAW_MODE_TRIANGLES;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return MALI_DRAW_MODE_TRIANGLE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return MALI_DRAW_MODE_TRIANGLE_FAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
   default:
      unreachable("Invalid primitive type");
   }
}

static VkResult
update_tls(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_tls_state *state = &cmdbuf->state.tls;
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   const struct panvk_shader *fs = cmdbuf->state.gfx.fs.shader;
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   if (!cmdbuf->state.gfx.tsd) {
      if (!state->desc.gpu) {
         state->desc = panvk_cmd_alloc_desc(cmdbuf, LOCAL_STORAGE);
         if (!state->desc.gpu)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }

      cmdbuf->state.gfx.tsd = state->desc.gpu;

      cs_update_vt_ctx(b)
         cs_move64_to(b, cs_sr_reg64(b, 24), state->desc.gpu);
   }

   state->info.tls.size =
      MAX3(vs->info.tls_size, fs ? fs->info.tls_size : 0, state->info.tls.size);
   return VK_SUCCESS;
}

static enum mali_index_type
index_size_to_index_type(uint32_t size)
{
   switch (size) {
   case 0:
      return MALI_INDEX_TYPE_NONE;
   case 1:
      return MALI_INDEX_TYPE_UINT8;
   case 2:
      return MALI_INDEX_TYPE_UINT16;
   case 4:
      return MALI_INDEX_TYPE_UINT32;
   default:
      assert(!"Invalid index size");
      return MALI_INDEX_TYPE_NONE;
   }
}

static VkResult
prepare_blend(struct panvk_cmd_buffer *cmdbuf)
{
   bool dirty = dyn_gfx_state_dirty(cmdbuf, CB_LOGIC_OP_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, CB_LOGIC_OP) ||
                dyn_gfx_state_dirty(cmdbuf, MS_ALPHA_TO_ONE_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, CB_ATTACHMENT_COUNT) ||
                dyn_gfx_state_dirty(cmdbuf, CB_COLOR_WRITE_ENABLES) ||
                dyn_gfx_state_dirty(cmdbuf, CB_BLEND_ENABLES) ||
                dyn_gfx_state_dirty(cmdbuf, CB_BLEND_EQUATIONS) ||
                dyn_gfx_state_dirty(cmdbuf, CB_WRITE_MASKS) ||
                dyn_gfx_state_dirty(cmdbuf, CB_BLEND_CONSTANTS);

   if (!dirty)
      return VK_SUCCESS;

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   const struct vk_dynamic_graphics_state *dyns =
      &cmdbuf->vk.dynamic_graphics_state;
   const struct vk_color_blend_state *cb = &dyns->cb;
   unsigned bd_count = MAX2(cb->attachment_count, 1);
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   const struct panvk_shader *fs = cmdbuf->state.gfx.fs.shader;
   const struct pan_shader_info *fs_info = fs ? &fs->info : NULL;
   mali_ptr fs_code = panvk_shader_get_dev_addr(fs);
   struct panfrost_ptr ptr =
      panvk_cmd_alloc_desc_array(cmdbuf, bd_count, BLEND);
   struct mali_blend_packed *bds = ptr.cpu;

   if (bd_count && !ptr.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   panvk_per_arch(blend_emit_descs)(
      dev, dyns, cmdbuf->state.gfx.render.color_attachments.fmts,
      cmdbuf->state.gfx.render.color_attachments.samples, fs_info, fs_code, bds,
      &cmdbuf->state.gfx.cb.info);

   cs_move64_to(b, cs_sr_reg64(b, 50), ptr.gpu | bd_count);
   return VK_SUCCESS;
}

static void
prepare_vp(struct panvk_cmd_buffer *cmdbuf)
{
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   const VkViewport *viewport =
      &cmdbuf->vk.dynamic_graphics_state.vp.viewports[0];
   const VkRect2D *scissor = &cmdbuf->vk.dynamic_graphics_state.vp.scissors[0];

   if (dyn_gfx_state_dirty(cmdbuf, VP_VIEWPORTS) ||
       dyn_gfx_state_dirty(cmdbuf, VP_SCISSORS)) {
      uint64_t scissor_box;
      pan_pack(&scissor_box, SCISSOR, cfg) {

         /* The spec says "width must be greater than 0.0" */
         assert(viewport->width >= 0);
         int minx = (int)viewport->x;
         int maxx = (int)(viewport->x + viewport->width);

         /* Viewport height can be negative */
         int miny =
            MIN2((int)viewport->y, (int)(viewport->y + viewport->height));
         int maxy =
            MAX2((int)viewport->y, (int)(viewport->y + viewport->height));

         assert(scissor->offset.x >= 0 && scissor->offset.y >= 0);
         minx = MAX2(scissor->offset.x, minx);
         miny = MAX2(scissor->offset.y, miny);
         maxx = MIN2(scissor->offset.x + scissor->extent.width, maxx);
         maxy = MIN2(scissor->offset.y + scissor->extent.height, maxy);

         /* Make sure we don't end up with a max < min when width/height is 0 */
         maxx = maxx > minx ? maxx - 1 : maxx;
         maxy = maxy > miny ? maxy - 1 : maxy;

         /* Clamp viewport scissor to valid range */
         cfg.scissor_minimum_x = CLAMP(minx, 0, UINT16_MAX);
         cfg.scissor_minimum_y = CLAMP(miny, 0, UINT16_MAX);
         cfg.scissor_maximum_x = CLAMP(maxx, 0, UINT16_MAX);
         cfg.scissor_maximum_y = CLAMP(maxy, 0, UINT16_MAX);
      }

      cs_move64_to(b, cs_sr_reg64(b, 42), scissor_box);
   }

   if (dyn_gfx_state_dirty(cmdbuf, VP_VIEWPORTS) ||
       dyn_gfx_state_dirty(cmdbuf, RS_CULL_MODE) ||
       dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_CLAMP_ENABLE)) {
      struct panvk_graphics_sysvals *sysvals = &cmdbuf->state.gfx.sysvals;

      float z_min = sysvals->viewport.offset.z;
      float z_max = z_min + sysvals->viewport.scale.z;
      cs_move32_to(b, cs_sr_reg32(b, 44), fui(MIN2(z_min, z_max)));
      cs_move32_to(b, cs_sr_reg32(b, 45), fui(MAX2(z_min, z_max)));
   }
}

static inline uint64_t
get_pos_spd(const struct panvk_cmd_buffer *cmdbuf)
{
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   assert(vs);
   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;
   return ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST
             ? panvk_priv_mem_dev_addr(vs->spds.pos_points)
             : panvk_priv_mem_dev_addr(vs->spds.pos_triangles);
}

static void
prepare_tiler_primitive_size(struct panvk_cmd_buffer *cmdbuf)
{
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   const struct vk_input_assembly_state *ia =
      &cmdbuf->vk.dynamic_graphics_state.ia;
   float primitive_size;

   if (!dyn_gfx_state_dirty(cmdbuf, IA_PRIMITIVE_TOPOLOGY) &&
       !dyn_gfx_state_dirty(cmdbuf, RS_LINE_WIDTH) &&
       !gfx_state_dirty(cmdbuf, VS))
      return;

   switch (ia->primitive_topology) {
   /* From the Vulkan spec 1.3.293:
    *
    *    "If maintenance5 is enabled and a value is not written to a variable
    *    decorated with PointSize, a value of 1.0 is used as the size of
    *    points."
    *
    * If no point size is written, ensure that the size is always 1.0f.
    */
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      if (vs->info.vs.writes_point_size)
         return;

      primitive_size = 1.0f;
      break;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      primitive_size = cmdbuf->vk.dynamic_graphics_state.rs.line.width;
      break;
   default:
      return;
   }

   cs_move32_to(b, cs_sr_reg32(b, 60), fui(primitive_size));
}

static uint32_t
calc_fbd_size(struct panvk_cmd_buffer *cmdbuf)
{
   const struct pan_fb_info *fb = &cmdbuf->state.gfx.render.fb.info;
   bool has_zs_ext = fb->zs.view.zs || fb->zs.view.s;
   uint32_t fbd_size = pan_size(FRAMEBUFFER);

   if (has_zs_ext)
      fbd_size += pan_size(ZS_CRC_EXTENSION);

   fbd_size += pan_size(RENDER_TARGET) * MAX2(fb->rt_count, 1);
   return fbd_size;
}

#define MAX_LAYERS_PER_TILER_DESC 8

static uint32_t
calc_render_descs_size(struct panvk_cmd_buffer *cmdbuf)
{
   uint32_t fbd_count = cmdbuf->state.gfx.render.layer_count;
   uint32_t td_count = DIV_ROUND_UP(cmdbuf->state.gfx.render.layer_count,
                                    MAX_LAYERS_PER_TILER_DESC);

   return (calc_fbd_size(cmdbuf) * fbd_count) +
          (td_count * pan_size(TILER_CONTEXT));
}

static void
cs_render_desc_ringbuf_reserve(struct cs_builder *b, uint32_t size)
{
   /* Make sure we don't allocate more than the ringbuf size. */
   assert(size <= RENDER_DESC_RINGBUF_SIZE);

   /* Make sure the allocation is 64-byte aligned. */
   assert(ALIGN_POT(size, 64) == size);

   struct cs_index ringbuf_sync = cs_scratch_reg64(b, 0);
   struct cs_index sz_reg = cs_scratch_reg32(b, 2);

   cs_load64_to(
      b, ringbuf_sync, cs_subqueue_ctx_reg(b),
      offsetof(struct panvk_cs_subqueue_context, render.desc_ringbuf.syncobj));
   cs_wait_slot(b, SB_ID(LS), false);

   /* Wait for the other end to release memory. */
   cs_move32_to(b, sz_reg, size - 1);
   cs_sync32_wait(b, false, MALI_CS_CONDITION_GREATER, sz_reg, ringbuf_sync);

   /* Decrement the syncobj to reflect the fact we're reserving memory. */
   cs_move32_to(b, sz_reg, -size);
   cs_sync32_add(b, false, MALI_CS_SYNC_SCOPE_CSG, sz_reg, ringbuf_sync,
                 cs_now());
}

static void
cs_render_desc_ringbuf_move_ptr(struct cs_builder *b, uint32_t size)
{
   struct cs_index scratch_reg = cs_scratch_reg32(b, 0);
   struct cs_index ptr_lo = cs_scratch_reg32(b, 2);
   struct cs_index pos = cs_scratch_reg32(b, 4);

   cs_load_to(
      b, cs_scratch_reg_tuple(b, 2, 3), cs_subqueue_ctx_reg(b),
      BITFIELD_MASK(3),
      offsetof(struct panvk_cs_subqueue_context, render.desc_ringbuf.ptr));
   cs_wait_slot(b, SB_ID(LS), false);

   /* Update the relative position and absolute address. */
   cs_add32(b, ptr_lo, ptr_lo, size);
   cs_add32(b, pos, pos, size);
   cs_add32(b, scratch_reg, pos, -RENDER_DESC_RINGBUF_SIZE);

   /* Wrap-around. */
   cs_if(b, MALI_CS_CONDITION_GEQUAL, scratch_reg) {
      cs_add32(b, ptr_lo, ptr_lo, -RENDER_DESC_RINGBUF_SIZE);
      cs_add32(b, pos, pos, -RENDER_DESC_RINGBUF_SIZE);
   }

   cs_store(
      b, cs_scratch_reg_tuple(b, 2, 3), cs_subqueue_ctx_reg(b),
      BITFIELD_MASK(3),
      offsetof(struct panvk_cs_subqueue_context, render.desc_ringbuf.ptr));
   cs_wait_slot(b, SB_ID(LS), false);
}

static VkResult
get_tiler_desc(struct panvk_cmd_buffer *cmdbuf)
{
   if (cmdbuf->state.gfx.render.tiler)
      return VK_SUCCESS;

   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(cmdbuf->vk.base.device->physical);
   struct panfrost_tiler_features tiler_features =
      panfrost_query_tiler_features(&phys_dev->kmod.props);
   bool simul_use =
      cmdbuf->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
   struct panfrost_ptr tiler_desc = {0};
   struct mali_tiler_context_packed tiler_tmpl;
   uint32_t td_count = DIV_ROUND_UP(cmdbuf->state.gfx.render.layer_count,
                                    MAX_LAYERS_PER_TILER_DESC);

   if (!simul_use) {
      tiler_desc = panvk_cmd_alloc_desc_array(cmdbuf, td_count, TILER_CONTEXT);
      if (!tiler_desc.gpu)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   pan_pack(&tiler_tmpl, TILER_CONTEXT, cfg) {
      unsigned max_levels = tiler_features.max_levels;
      assert(max_levels >= 2);

      cfg.hierarchy_mask =
         panvk_select_tiler_hierarchy_mask(phys_dev, &cmdbuf->state.gfx);
      cfg.fb_width = cmdbuf->state.gfx.render.fb.info.width;
      cfg.fb_height = cmdbuf->state.gfx.render.fb.info.height;

      cfg.sample_pattern =
         pan_sample_pattern(cmdbuf->state.gfx.render.fb.info.nr_samples);

      /* TODO: revisit for VK_EXT_provoking_vertex. */
      cfg.first_provoking_vertex = true;

      /* This will be overloaded. */
      cfg.layer_count = 1;
      cfg.layer_offset = 0;
   }

   cmdbuf->state.gfx.render.tiler =
      simul_use ? 0xdeadbeefdeadbeefull : tiler_desc.gpu;

   struct cs_index tiler_ctx_addr = cs_sr_reg64(b, 40);

   if (simul_use) {
      uint32_t descs_sz = calc_render_descs_size(cmdbuf);

      cs_render_desc_ringbuf_reserve(b, descs_sz);

      /* Reserve ringbuf mem. */
      cs_update_vt_ctx(b) {
         cs_load64_to(b, tiler_ctx_addr, cs_subqueue_ctx_reg(b),
                      offsetof(struct panvk_cs_subqueue_context,
                               render.desc_ringbuf.ptr));
      }

      cs_render_desc_ringbuf_move_ptr(b, descs_sz);
   } else {
      cs_update_vt_ctx(b) {
         cs_move64_to(b, tiler_ctx_addr, tiler_desc.gpu);
      }
   }

   /* Reset the polygon list. */
   cs_move64_to(b, cs_scratch_reg64(b, 0), 0);

   /* Lay out words 2, 3 and 5, so they can be stored along the other updates.
    * Word 4 contains layer information and will be updated in the loop. */
   cs_move64_to(b, cs_scratch_reg64(b, 2),
                tiler_tmpl.opaque[2] | (uint64_t)tiler_tmpl.opaque[3] << 32);
   cs_move32_to(b, cs_scratch_reg32(b, 5), tiler_tmpl.opaque[5]);

   /* Load the tiler_heap and geom_buf from the context. */
   cs_load_to(b, cs_scratch_reg_tuple(b, 6, 4), cs_subqueue_ctx_reg(b),
              BITFIELD_MASK(4),
              offsetof(struct panvk_cs_subqueue_context, render.tiler_heap));

   /* Fill extra fields with zeroes so we can reset the completed
    * top/bottom and private states. */
   cs_move64_to(b, cs_scratch_reg64(b, 10), 0);
   cs_move64_to(b, cs_scratch_reg64(b, 12), 0);
   cs_move64_to(b, cs_scratch_reg64(b, 14), 0);

   cs_wait_slot(b, SB_ID(LS), false);

   /* Take care of the tiler desc with layer_offset=0 outside of the loop. */
   cs_move32_to(b, cs_scratch_reg32(b, 4),
                MIN2(cmdbuf->state.gfx.render.layer_count - 1,
                     MAX_LAYERS_PER_TILER_DESC - 1));

   /* Replace words 0:13 and 24:31. */
   cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
            BITFIELD_MASK(16), 0);
   cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
            BITFIELD_RANGE(0, 2) | BITFIELD_RANGE(10, 6), 64);
   cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
            BITFIELD_RANGE(0, 2) | BITFIELD_RANGE(10, 6), 96);

   cs_wait_slot(b, SB_ID(LS), false);

   uint32_t remaining_layers =
      td_count > 1
         ? cmdbuf->state.gfx.render.layer_count % MAX_LAYERS_PER_TILER_DESC
         : 0;
   uint32_t full_td_count =
      cmdbuf->state.gfx.render.layer_count / MAX_LAYERS_PER_TILER_DESC;

   if (remaining_layers) {
      int32_t layer_offset =
         -(cmdbuf->state.gfx.render.layer_count - remaining_layers) &
         BITFIELD_MASK(9);

      /* If the last tiler descriptor is not full, we emit it outside of the
       * loop to pass the right layer count. All this would be a lot simpler
       * if we had OR/AND instructions, but here we are. */
      cs_update_vt_ctx(b)
         cs_add64(b, tiler_ctx_addr, tiler_ctx_addr,
                  pan_size(TILER_CONTEXT) * full_td_count);
      cs_move32_to(b, cs_scratch_reg32(b, 4),
                   (layer_offset << 8) | (remaining_layers - 1));
      cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
               BITFIELD_MASK(16), 0);
      cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
               BITFIELD_RANGE(0, 2) | BITFIELD_RANGE(10, 6), 64);
      cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
               BITFIELD_RANGE(0, 2) | BITFIELD_RANGE(10, 6), 96);
      cs_wait_slot(b, SB_ID(LS), false);

      cs_update_vt_ctx(b)
         cs_add64(b, tiler_ctx_addr, tiler_ctx_addr,
                  -pan_size(TILER_CONTEXT));
   } else if (full_td_count > 1) {
      cs_update_vt_ctx(b)
         cs_add64(b, tiler_ctx_addr, tiler_ctx_addr,
                  pan_size(TILER_CONTEXT) * (full_td_count - 1));
   }

   if (full_td_count > 1) {
      struct cs_index counter_reg = cs_scratch_reg32(b, 17);
      uint32_t layer_offset =
         (-MAX_LAYERS_PER_TILER_DESC * (full_td_count - 1)) & BITFIELD_MASK(9);

      cs_move32_to(b, counter_reg, full_td_count - 1);
      cs_move32_to(b, cs_scratch_reg32(b, 4),
                   (layer_offset << 8) | (MAX_LAYERS_PER_TILER_DESC - 1));

      /* We iterate the remaining full tiler descriptors in reverse order, so we
       * can start from the smallest layer offset, and increment it by
       * MAX_LAYERS_PER_TILER_DESC << 8 at each iteration. Again, the split is
       * mostly due to the lack of AND instructions, and the fact layer_offset
       * is a 9-bit signed integer inside a 32-bit word, which ADD32 can't deal
       * with unless the number we add is positive.
       */
      cs_while(b, MALI_CS_CONDITION_GREATER, counter_reg) {
         /* Replace words 0:13 and 24:31. */
         cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
                  BITFIELD_MASK(16), 0);
         cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
                  BITFIELD_RANGE(0, 2) | BITFIELD_RANGE(10, 6), 64);
         cs_store(b, cs_scratch_reg_tuple(b, 0, 16), tiler_ctx_addr,
                  BITFIELD_RANGE(0, 2) | BITFIELD_RANGE(10, 6), 96);

         cs_wait_slot(b, SB_ID(LS), false);

         cs_add32(b, cs_scratch_reg32(b, 4), cs_scratch_reg32(b, 4),
                  MAX_LAYERS_PER_TILER_DESC << 8);

         cs_add32(b, counter_reg, counter_reg, -1);
         cs_update_vt_ctx(b)
            cs_add64(b, tiler_ctx_addr, tiler_ctx_addr,
                     -pan_size(TILER_CONTEXT));
      }
   }

   /* Then we change the scoreboard slot used for iterators. */
   panvk_per_arch(cs_pick_iter_sb)(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   cs_heap_operation(b, MALI_CS_HEAP_OPERATION_VERTEX_TILER_STARTED, cs_now());
   return VK_SUCCESS;
}

static VkResult
get_fb_descs(struct panvk_cmd_buffer *cmdbuf)
{
   if (cmdbuf->state.gfx.render.fbds.gpu ||
       !cmdbuf->state.gfx.render.layer_count)
      return VK_SUCCESS;

   uint32_t fbds_sz =
      calc_fbd_size(cmdbuf) * cmdbuf->state.gfx.render.layer_count;

   cmdbuf->state.gfx.render.fbds = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, fbds_sz, pan_alignment(FRAMEBUFFER));
   if (!cmdbuf->state.gfx.render.fbds.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   return VK_SUCCESS;
}

static VkResult
prepare_vs(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   struct panvk_shader_desc_state *vs_desc_state = &cmdbuf->state.gfx.vs.desc;
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   bool upd_res_table = false;

   VkResult result = prepare_vs_driver_set(cmdbuf);
   if (result != VK_SUCCESS)
      return result;

   if (gfx_state_dirty(cmdbuf, VS) || gfx_state_dirty(cmdbuf, DESC_STATE) ||
       vs_driver_set_is_dirty(cmdbuf)) {
      result = panvk_per_arch(cmd_prepare_shader_res_table)(cmdbuf, desc_state,
                                                            vs, vs_desc_state);
      if (result != VK_SUCCESS)
         return result;

      upd_res_table = true;
   }

   cs_update_vt_ctx(b) {
      if (upd_res_table)
         cs_move64_to(b, cs_sr_reg64(b, 0), vs_desc_state->res_table);

      if (gfx_state_dirty(cmdbuf, VS) ||
          dyn_gfx_state_dirty(cmdbuf, IA_PRIMITIVE_TOPOLOGY))
         cs_move64_to(b, cs_sr_reg64(b, 16), get_pos_spd(cmdbuf));

      if (gfx_state_dirty(cmdbuf, VS))
         cs_move64_to(b, cs_sr_reg64(b, 18),
                      panvk_priv_mem_dev_addr(vs->spds.var));
   }

   return VK_SUCCESS;
}

static VkResult
prepare_fs(struct panvk_cmd_buffer *cmdbuf)
{
   const struct panvk_shader *fs = get_fs(cmdbuf);
   struct panvk_shader_desc_state *fs_desc_state = &cmdbuf->state.gfx.fs.desc;
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   if (fs &&
       (gfx_state_dirty(cmdbuf, FS) || gfx_state_dirty(cmdbuf, DESC_STATE))) {
      VkResult result = prepare_fs_driver_set(cmdbuf);
      if (result != VK_SUCCESS)
         return result;

      result = panvk_per_arch(cmd_prepare_shader_res_table)(cmdbuf, desc_state,
                                                            fs, fs_desc_state);
      if (result != VK_SUCCESS)
         return result;
   }

   cs_update_vt_ctx(b) {
      if (fs_user_dirty(cmdbuf) || gfx_state_dirty(cmdbuf, DESC_STATE))
         cs_move64_to(b, cs_sr_reg64(b, 4), fs ? fs_desc_state->res_table : 0);
      if (fs_user_dirty(cmdbuf))
         cs_move64_to(b, cs_sr_reg64(b, 20),
                      fs ? panvk_priv_mem_dev_addr(fs->spd) : 0);
   }

   return VK_SUCCESS;
}

static VkResult
prepare_push_uniforms(struct panvk_cmd_buffer *cmdbuf)
{
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   if (gfx_state_dirty(cmdbuf, PUSH_UNIFORMS)) {
      cmdbuf->state.gfx.push_uniforms = panvk_per_arch(
         cmd_prepare_push_uniforms)(cmdbuf, &cmdbuf->state.gfx.sysvals,
                                    sizeof(cmdbuf->state.gfx.sysvals));
      if (!cmdbuf->state.gfx.push_uniforms)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      uint32_t push_size = 256 + sizeof(struct panvk_graphics_sysvals);
      uint64_t fau_count = DIV_ROUND_UP(push_size, 8);
      mali_ptr fau_ptr = cmdbuf->state.gfx.push_uniforms | (fau_count << 56);

      cs_update_vt_ctx(b) {
         cs_move64_to(b, cs_sr_reg64(b, 8), fau_ptr);
         cs_move64_to(b, cs_sr_reg64(b, 12), fau_ptr);
      }
   }

   return VK_SUCCESS;
}

static VkResult
prepare_ds(struct panvk_cmd_buffer *cmdbuf)
{
   bool dirty = dyn_gfx_state_dirty(cmdbuf, DS_DEPTH_TEST_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, DS_DEPTH_WRITE_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, DS_DEPTH_COMPARE_OP) ||
                dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_TEST_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_OP) ||
                dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_COMPARE_MASK) ||
                dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_WRITE_MASK) ||
                dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_REFERENCE) ||
                dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_CLAMP_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_CLIP_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_BIAS_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_BIAS_FACTORS) ||
                fs_user_dirty(cmdbuf);

   if (!dirty)
      return VK_SUCCESS;

   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   const struct vk_dynamic_graphics_state *dyns =
      &cmdbuf->vk.dynamic_graphics_state;
   const struct vk_depth_stencil_state *ds = &dyns->ds;
   const struct vk_rasterization_state *rs = &dyns->rs;
   bool test_s = has_stencil_att(cmdbuf) && ds->stencil.test_enable;
   bool test_z = has_depth_att(cmdbuf) && ds->depth.test_enable;
   const struct panvk_shader *fs = get_fs(cmdbuf);

   struct panfrost_ptr zsd = panvk_cmd_alloc_desc(cmdbuf, DEPTH_STENCIL);
   if (!zsd.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   pan_pack(zsd.cpu, DEPTH_STENCIL, cfg) {
      cfg.stencil_test_enable = test_s;
      if (test_s) {
         cfg.front_compare_function =
            translate_compare_func(ds->stencil.front.op.compare);
         cfg.front_stencil_fail =
            translate_stencil_op(ds->stencil.front.op.fail);
         cfg.front_depth_fail =
            translate_stencil_op(ds->stencil.front.op.depth_fail);
         cfg.front_depth_pass = translate_stencil_op(ds->stencil.front.op.pass);
         cfg.back_compare_function =
            translate_compare_func(ds->stencil.back.op.compare);
         cfg.back_stencil_fail = translate_stencil_op(ds->stencil.back.op.fail);
         cfg.back_depth_fail =
            translate_stencil_op(ds->stencil.back.op.depth_fail);
         cfg.back_depth_pass = translate_stencil_op(ds->stencil.back.op.pass);
      }

      cfg.stencil_from_shader = fs ? fs->info.fs.writes_stencil : 0;
      cfg.front_write_mask = ds->stencil.front.write_mask;
      cfg.back_write_mask = ds->stencil.back.write_mask;
      cfg.front_value_mask = ds->stencil.front.compare_mask;
      cfg.back_value_mask = ds->stencil.back.compare_mask;
      cfg.front_reference_value = ds->stencil.front.reference;
      cfg.back_reference_value = ds->stencil.back.reference;

      cfg.depth_cull_enable = vk_rasterization_state_depth_clip_enable(rs);
      if (rs->depth_clamp_enable)
         cfg.depth_clamp_mode = MALI_DEPTH_CLAMP_MODE_BOUNDS;

      if (fs)
         cfg.depth_source = pan_depth_source(&fs->info);
      cfg.depth_write_enable = test_z && ds->depth.write_enable;
      cfg.depth_bias_enable = rs->depth_bias.enable;
      cfg.depth_function = test_z ? translate_compare_func(ds->depth.compare_op)
                                  : MALI_FUNC_ALWAYS;
      cfg.depth_units = rs->depth_bias.constant * 2.0f;
      cfg.depth_factor = rs->depth_bias.slope;
      cfg.depth_bias_clamp = rs->depth_bias.clamp;
   }

   cs_update_vt_ctx(b)
      cs_move64_to(b, cs_sr_reg64(b, 52), zsd.gpu);

   return VK_SUCCESS;
}

static void
prepare_dcd(struct panvk_cmd_buffer *cmdbuf)
{
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   const struct panvk_shader *fs = get_fs(cmdbuf);
   bool dcd0_dirty =
      dyn_gfx_state_dirty(cmdbuf, RS_RASTERIZER_DISCARD_ENABLE) ||
      dyn_gfx_state_dirty(cmdbuf, RS_CULL_MODE) ||
      dyn_gfx_state_dirty(cmdbuf, RS_FRONT_FACE) ||
      dyn_gfx_state_dirty(cmdbuf, MS_RASTERIZATION_SAMPLES) ||
      dyn_gfx_state_dirty(cmdbuf, MS_SAMPLE_MASK) ||
      dyn_gfx_state_dirty(cmdbuf, MS_ALPHA_TO_COVERAGE_ENABLE) ||
      dyn_gfx_state_dirty(cmdbuf, MS_ALPHA_TO_ONE_ENABLE) ||
      /* writes_depth() uses vk_depth_stencil_state */
      dyn_gfx_state_dirty(cmdbuf, DS_DEPTH_TEST_ENABLE) ||
      dyn_gfx_state_dirty(cmdbuf, DS_DEPTH_WRITE_ENABLE) ||
      dyn_gfx_state_dirty(cmdbuf, DS_DEPTH_COMPARE_OP) ||
      /* writes_stencil() uses vk_depth_stencil_state */
      dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_TEST_ENABLE) ||
      dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_OP) ||
      dyn_gfx_state_dirty(cmdbuf, DS_STENCIL_WRITE_MASK) ||
      fs_user_dirty(cmdbuf) || gfx_state_dirty(cmdbuf, RENDER_STATE);
   bool dcd1_dirty = dyn_gfx_state_dirty(cmdbuf, MS_RASTERIZATION_SAMPLES) ||
                     dyn_gfx_state_dirty(cmdbuf, MS_SAMPLE_MASK) ||
                     fs_user_dirty(cmdbuf) ||
                     gfx_state_dirty(cmdbuf, RENDER_STATE);

   const struct vk_dynamic_graphics_state *dyns =
      &cmdbuf->vk.dynamic_graphics_state;
   const struct vk_rasterization_state *rs =
      &cmdbuf->vk.dynamic_graphics_state.rs;
   bool alpha_to_coverage = dyns->ms.alpha_to_coverage_enable;
   bool writes_z = writes_depth(cmdbuf);
   bool writes_s = writes_stencil(cmdbuf);

   if (dcd0_dirty) {
      struct mali_dcd_flags_0_packed dcd0;
      pan_pack(&dcd0, DCD_FLAGS_0, cfg) {
         if (fs) {
            uint8_t rt_written = fs->info.outputs_written >> FRAG_RESULT_DATA0;
            uint8_t rt_mask = cmdbuf->state.gfx.render.bound_attachments &
                              MESA_VK_RP_ATTACHMENT_ANY_COLOR_BITS;

            cfg.allow_forward_pixel_to_kill =
               fs->info.fs.can_fpk && !(rt_mask & ~rt_written) &&
               !alpha_to_coverage && !cmdbuf->state.gfx.cb.info.any_dest_read;

            bool writes_zs = writes_z || writes_s;
            bool zs_always_passes = ds_test_always_passes(cmdbuf);
            bool oq = false; /* TODO: Occlusion queries */

            struct pan_earlyzs_state earlyzs =
               pan_earlyzs_get(pan_earlyzs_analyze(&fs->info), writes_zs || oq,
                               alpha_to_coverage, zs_always_passes);

            cfg.pixel_kill_operation = earlyzs.kill;
            cfg.zs_update_operation = earlyzs.update;
         } else {
            cfg.allow_forward_pixel_to_kill = true;
            cfg.allow_forward_pixel_to_be_killed = true;
            cfg.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
            cfg.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
            cfg.overdraw_alpha0 = true;
            cfg.overdraw_alpha1 = true;
         }

         cfg.front_face_ccw = rs->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;
         cfg.cull_front_face = (rs->cull_mode & VK_CULL_MODE_FRONT_BIT) != 0;
         cfg.cull_back_face = (rs->cull_mode & VK_CULL_MODE_BACK_BIT) != 0;

         cfg.multisample_enable = dyns->ms.rasterization_samples > 1;
      }

      cs_update_vt_ctx(b)
         cs_move32_to(b, cs_sr_reg32(b, 57), dcd0.opaque[0]);
   }

   if (dcd1_dirty) {
      struct mali_dcd_flags_1_packed dcd1;
      pan_pack(&dcd1, DCD_FLAGS_1, cfg) {
         cfg.sample_mask = dyns->ms.rasterization_samples > 1
                              ? dyns->ms.sample_mask
                              : UINT16_MAX;

         if (fs) {
            cfg.render_target_mask =
               (fs->info.outputs_written >> FRAG_RESULT_DATA0) &
               cmdbuf->state.gfx.render.bound_attachments;
         }
      }

      cs_update_vt_ctx(b)
         cs_move32_to(b, cs_sr_reg32(b, 58), dcd1.opaque[0]);
   }
}

static void
prepare_index_buffer(struct panvk_cmd_buffer *cmdbuf,
                     struct panvk_draw_info *draw)
{
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   if (draw->index.size && gfx_state_dirty(cmdbuf, IB)) {
      uint64_t ib_size =
         panvk_buffer_range(cmdbuf->state.gfx.ib.buffer,
                            cmdbuf->state.gfx.ib.offset, VK_WHOLE_SIZE);
      assert(ib_size <= UINT32_MAX);
      cs_move32_to(b, cs_sr_reg32(b, 39), ib_size);

      cs_move64_to(b, cs_sr_reg64(b, 54),
                   panvk_buffer_gpu_ptr(cmdbuf->state.gfx.ib.buffer,
                                        cmdbuf->state.gfx.ib.offset));
   }
}

static void
set_tiler_idvs_flags(struct cs_builder *b, struct panvk_cmd_buffer *cmdbuf,
                     struct panvk_draw_info *draw)
{
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   const struct panvk_shader *fs = get_fs(cmdbuf);
   const struct vk_dynamic_graphics_state *dyns =
      &cmdbuf->vk.dynamic_graphics_state;
   const struct vk_input_assembly_state *ia = &dyns->ia;
   const struct vk_rasterization_state *rs = &dyns->rs;
   struct mali_primitive_flags_packed tiler_idvs_flags;
   bool writes_point_size =
      vs->info.vs.writes_point_size &&
      ia->primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

   bool dirty = gfx_state_dirty(cmdbuf, VS) || fs_user_dirty(cmdbuf) ||
                dyn_gfx_state_dirty(cmdbuf, IA_PRIMITIVE_RESTART_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, IA_PRIMITIVE_TOPOLOGY) ||
                dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_CLAMP_ENABLE) ||
                dyn_gfx_state_dirty(cmdbuf, RS_DEPTH_CLIP_ENABLE);

   if (dirty) {
      pan_pack(&tiler_idvs_flags, PRIMITIVE_FLAGS, cfg) {
         cfg.draw_mode = translate_prim_topology(ia->primitive_topology);

         if (writes_point_size) {
            cfg.point_size_array_format = MALI_POINT_SIZE_ARRAY_FORMAT_FP16;
            cfg.position_fifo_format = MALI_FIFO_FORMAT_EXTENDED;
         } else {
            cfg.point_size_array_format = MALI_POINT_SIZE_ARRAY_FORMAT_NONE;
            cfg.position_fifo_format = MALI_FIFO_FORMAT_BASIC;
         }

         if (vs->info.outputs_written & VARYING_BIT_LAYER) {
            cfg.layer_index_enable = true;
            cfg.position_fifo_format = MALI_FIFO_FORMAT_EXTENDED;
         }

         cfg.low_depth_cull = cfg.high_depth_cull =
            vk_rasterization_state_depth_clip_enable(rs);

         cfg.secondary_shader = vs->info.vs.secondary_enable && fs != NULL;
         cfg.primitive_restart = ia->primitive_restart_enable;
      }

      cs_move32_to(b, cs_sr_reg32(b, 56), tiler_idvs_flags.opaque[0]);
   }
}

static struct mali_primitive_flags_packed
get_tiler_flags_override(struct panvk_draw_info *draw)
{
   struct mali_primitive_flags_packed flags_override;
   /* Pack with nodefaults so only explicitly set override fields affect the
    * previously set register values */
   pan_pack_nodefaults(&flags_override, PRIMITIVE_FLAGS, cfg) {
      cfg.index_type = index_size_to_index_type(draw->index.size);
   };

   return flags_override;
}

static VkResult
prepare_draw(struct panvk_cmd_buffer *cmdbuf, struct panvk_draw_info *draw)
{
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   const struct panvk_shader *fs = get_fs(cmdbuf);
   struct panvk_descriptor_state *desc_state = &cmdbuf->state.gfx.desc_state;
   bool idvs = vs->info.vs.idvs;
   VkResult result;

   assert(vs);

   /* FIXME: support non-IDVS. */
   assert(idvs);

   result = update_tls(cmdbuf);
   if (result != VK_SUCCESS)
      return result;

   if (cmdbuf->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
       !(cmdbuf->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
      result = get_tiler_desc(cmdbuf);
      if (result != VK_SUCCESS)
         return result;

      result = get_fb_descs(cmdbuf);
      if (result != VK_SUCCESS)
         return result;
   }

   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   uint32_t used_set_mask =
      vs->desc_info.used_set_mask | (fs ? fs->desc_info.used_set_mask : 0);

   if (gfx_state_dirty(cmdbuf, DESC_STATE) || gfx_state_dirty(cmdbuf, VS) ||
       gfx_state_dirty(cmdbuf, FS)) {
      result = panvk_per_arch(cmd_prepare_push_descs)(cmdbuf, desc_state,
                                                      used_set_mask);
      if (result != VK_SUCCESS)
         return result;
   }

   prepare_sysvals(cmdbuf);

   result = prepare_push_uniforms(cmdbuf);
   if (result != VK_SUCCESS)
      return result;

   result = prepare_vs(cmdbuf);
   if (result != VK_SUCCESS)
      return result;

   result = prepare_fs(cmdbuf);
   if (result != VK_SUCCESS)
      return result;

   uint32_t varying_size = 0;

   if (fs) {
      unsigned vs_vars = vs->info.varyings.output_count;
      unsigned fs_vars = fs->info.varyings.input_count;
      unsigned var_slots = MAX2(vs_vars, fs_vars);

      /* Assumes 16 byte slots. We could do better. */
      varying_size = var_slots * 16;
   }

   cs_update_vt_ctx(b) {
      /* We don't use the resource dep system yet. */
      cs_move32_to(b, cs_sr_reg32(b, 38), 0);

      prepare_index_buffer(cmdbuf, draw);

      set_tiler_idvs_flags(b, cmdbuf, draw);

      cs_move32_to(b, cs_sr_reg32(b, 48), varying_size);

      result = prepare_blend(cmdbuf);
      if (result != VK_SUCCESS)
         return result;

      result = prepare_ds(cmdbuf);
      if (result != VK_SUCCESS)
         return result;

      prepare_dcd(cmdbuf);
      prepare_vp(cmdbuf);
      prepare_tiler_primitive_size(cmdbuf);
   }

   clear_dirty_after_draw(cmdbuf);
   return VK_SUCCESS;
}

static void
panvk_cmd_draw(struct panvk_cmd_buffer *cmdbuf, struct panvk_draw_info *draw)
{
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   VkResult result;

   /* If there's no vertex shader, we can skip the draw. */
   if (!panvk_priv_mem_dev_addr(vs->spds.pos_points))
      return;

   /* Needs to be done before get_fs() is called because it depends on
    * fs.required being initialized. */
   cmdbuf->state.gfx.fs.required =
      fs_required(&cmdbuf->state.gfx, &cmdbuf->vk.dynamic_graphics_state);

   result = prepare_draw(cmdbuf, draw);
   if (result != VK_SUCCESS)
      return;

   cs_update_vt_ctx(b) {
      cs_move32_to(b, cs_sr_reg32(b, 32), 0);
      cs_move32_to(b, cs_sr_reg32(b, 33), draw->vertex.count);
      cs_move32_to(b, cs_sr_reg32(b, 34), draw->instance.count);
      cs_move32_to(b, cs_sr_reg32(b, 35), draw->index.offset);
      cs_move32_to(b, cs_sr_reg32(b, 36), draw->vertex.base);
      cs_move32_to(b, cs_sr_reg32(b, 37), draw->instance.base);
   }

   struct mali_primitive_flags_packed flags_override =
      get_tiler_flags_override(draw);

   uint32_t idvs_count = DIV_ROUND_UP(cmdbuf->state.gfx.render.layer_count,
                                      MAX_LAYERS_PER_TILER_DESC);

   cs_req_res(b, CS_IDVS_RES);
   if (idvs_count > 1) {
      struct cs_index counter_reg = cs_scratch_reg32(b, 17);
      struct cs_index tiler_ctx_addr = cs_sr_reg64(b, 40);

      cs_move32_to(b, counter_reg, idvs_count);

      cs_while(b, MALI_CS_CONDITION_GREATER, counter_reg) {
         cs_run_idvs(b, flags_override.opaque[0], false, true,
                     cs_shader_res_sel(0, 0, 1, 0),
                     cs_shader_res_sel(2, 2, 2, 0), cs_undef());

	 cs_add32(b, counter_reg, counter_reg, -1);
         cs_update_vt_ctx(b) {
            cs_add64(b, tiler_ctx_addr, tiler_ctx_addr,
                     pan_size(TILER_CONTEXT));
         }
      }

      cs_update_vt_ctx(b) {
         cs_add64(b, tiler_ctx_addr, tiler_ctx_addr,
                  -(idvs_count * pan_size(TILER_CONTEXT)));
      }
   } else {
      cs_run_idvs(b, flags_override.opaque[0], false, true,
                  cs_shader_res_sel(0, 0, 1, 0), cs_shader_res_sel(2, 2, 2, 0),
                  cs_undef());
   }
   cs_req_res(b, 0);
}

void
panvk_per_arch(cmd_prepare_exec_cmd_for_draws)(
   struct panvk_cmd_buffer *primary,
   struct panvk_cmd_buffer *secondary)
{
   VkResult result;

   if (secondary->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
      result = get_tiler_desc(primary);
      if (result != VK_SUCCESS)
         return;

      result = get_fb_descs(primary);
      if (result != VK_SUCCESS)
         return;
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                        uint32_t instanceCount, uint32_t firstVertex,
                        uint32_t firstInstance)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (instanceCount == 0 || vertexCount == 0)
      return;

   struct panvk_draw_info draw = {
      .vertex.base = firstVertex,
      .vertex.count = vertexCount,
      .instance.base = firstInstance,
      .instance.count = instanceCount,
   };

   panvk_cmd_draw(cmdbuf, &draw);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDrawIndexed)(VkCommandBuffer commandBuffer,
                               uint32_t indexCount, uint32_t instanceCount,
                               uint32_t firstIndex, int32_t vertexOffset,
                               uint32_t firstInstance)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (instanceCount == 0 || indexCount == 0)
      return;

   struct panvk_draw_info draw = {
      .index.size = cmdbuf->state.gfx.ib.index_size,
      .index.offset = firstIndex,
      .vertex.base = (uint32_t)vertexOffset,
      .vertex.count = indexCount,
      .instance.count = instanceCount,
      .instance.base = firstInstance,
   };

   panvk_cmd_draw(cmdbuf, &draw);
}

static void
panvk_cmd_draw_indirect(struct panvk_cmd_buffer *cmdbuf,
                        struct panvk_draw_info *draw)
{
   const struct panvk_shader *vs = cmdbuf->state.gfx.vs.shader;
   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   VkResult result;

   /* If there's no vertex shader, we can skip the draw. */
   if (!panvk_priv_mem_dev_addr(vs->spds.pos_points))
      return;

   /* Needs to be done before get_fs() is called because it depends on
    * fs.required being initialized. */
   cmdbuf->state.gfx.fs.required =
      fs_required(&cmdbuf->state.gfx, &cmdbuf->vk.dynamic_graphics_state);

   /* Layered indirect draw (VK_EXT_shader_viewport_index_layer) needs
    * additional changes. We allow layer_count == 0 because that happens
    * when mixing dynamic rendering and secondary command buffers. Once
    * we decide to support layared+indirect, we'll need to pass the
    * layer_count info through the tiler descriptor, for instance by
    * re-using one of the word that's flagged 'ignored' in the descriptor
    * (word 14:23). */
   assert(cmdbuf->state.gfx.render.layer_count <= 1);

   /* MultiDrawIndirect (.maxDrawIndirectCount) needs additional changes. */
   assert(draw->indirect.draw_count == 1);

   result = prepare_draw(cmdbuf, draw);
   if (result != VK_SUCCESS)
      return;

   struct cs_index draw_params_addr = cs_scratch_reg64(b, 0);
   cs_move64_to(
      b, draw_params_addr,
      panvk_buffer_gpu_ptr(draw->indirect.buffer, draw->indirect.offset));

   cs_update_vt_ctx(b) {
      cs_move32_to(b, cs_sr_reg32(b, 32), 0);
      /* Load SR33-37 from indirect buffer. */
      unsigned reg_mask = draw->index.size ? 0b11111 : 0b11011;
      cs_load_to(b, cs_sr_reg_tuple(b, 33, 5), draw_params_addr, reg_mask, 0);
   }

   struct mali_primitive_flags_packed flags_override =
      get_tiler_flags_override(draw);

   /* Wait for the SR33-37 indirect buffer load. */
   cs_wait_slot(b, SB_ID(LS), false);

   cs_req_res(b, CS_IDVS_RES);
   cs_run_idvs(b, flags_override.opaque[0], false, true,
               cs_shader_res_sel(0, 0, 1, 0), cs_shader_res_sel(2, 2, 2, 0),
               cs_undef());
   cs_req_res(b, 0);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDrawIndirect)(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                VkDeviceSize offset, uint32_t drawCount,
                                uint32_t stride)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_buffer, buffer, _buffer);

   if (drawCount == 0)
      return;

   struct panvk_draw_info draw = {
      .indirect.buffer = buffer,
      .indirect.offset = offset,
      .indirect.draw_count = drawCount,
      .indirect.stride = stride,
   };

   panvk_cmd_draw_indirect(cmdbuf, &draw);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDrawIndexedIndirect)(VkCommandBuffer commandBuffer,
                                       VkBuffer _buffer, VkDeviceSize offset,
                                       uint32_t drawCount, uint32_t stride)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_buffer, buffer, _buffer);

   if (drawCount == 0)
      return;

   struct panvk_draw_info draw = {
      .index.size = cmdbuf->state.gfx.ib.index_size,
      .indirect.buffer = buffer,
      .indirect.offset = offset,
      .indirect.draw_count = drawCount,
      .indirect.stride = stride,
   };

   panvk_cmd_draw_indirect(cmdbuf, &draw);
}

void
panvk_per_arch(cmd_inherit_render_state)(
   struct panvk_cmd_buffer *cmdbuf,
   const VkCommandBufferBeginInfo *pBeginInfo)
{
   if (cmdbuf->vk.level != VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
       !(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT))
      return;

   assert(pBeginInfo->pInheritanceInfo);
   char gcbiar_data[VK_GCBIARR_DATA_SIZE(MAX_RTS)];
   const VkRenderingInfo *resume_info =
      vk_get_command_buffer_inheritance_as_rendering_resume(cmdbuf->vk.level,
                                                            pBeginInfo,
                                                            gcbiar_data);
   if (resume_info) {
      panvk_per_arch(cmd_init_render_state)(cmdbuf, resume_info);
      return;
   }

   const VkCommandBufferInheritanceRenderingInfo *inheritance_info =
      vk_get_command_buffer_inheritance_rendering_info(cmdbuf->vk.level,
                                                       pBeginInfo);
   assert(inheritance_info);
   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.render.fb.info;

   cmdbuf->state.gfx.render.flags = inheritance_info->flags;

   gfx_state_set_dirty(cmdbuf, RENDER_STATE);
   memset(cmdbuf->state.gfx.render.fb.crc_valid, 0,
          sizeof(cmdbuf->state.gfx.render.fb.crc_valid));
   memset(&cmdbuf->state.gfx.render.color_attachments, 0,
          sizeof(cmdbuf->state.gfx.render.color_attachments));
   memset(&cmdbuf->state.gfx.render.z_attachment, 0,
          sizeof(cmdbuf->state.gfx.render.z_attachment));
   memset(&cmdbuf->state.gfx.render.s_attachment, 0,
          sizeof(cmdbuf->state.gfx.render.s_attachment));
   cmdbuf->state.gfx.render.bound_attachments = 0;

   cmdbuf->state.gfx.render.layer_count = 0;
   *fbinfo = (struct pan_fb_info){
      .tile_buf_budget = panfrost_query_optimal_tib_size(phys_dev->model),
      .nr_samples = 1,
      .rt_count = inheritance_info->colorAttachmentCount,
   };

   assert(inheritance_info->colorAttachmentCount <= ARRAY_SIZE(fbinfo->rts));

   for (uint32_t i = 0; i < inheritance_info->colorAttachmentCount; i++) {
      cmdbuf->state.gfx.render.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_COLOR_BIT(i);
      cmdbuf->state.gfx.render.color_attachments.fmts[i] =
         inheritance_info->pColorAttachmentFormats[i];
      cmdbuf->state.gfx.render.color_attachments.samples[i] =
         inheritance_info->rasterizationSamples;
   }

   if (inheritance_info->depthAttachmentFormat) {
      cmdbuf->state.gfx.render.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_DEPTH_BIT;
      cmdbuf->state.gfx.render.z_attachment.fmt =
         inheritance_info->depthAttachmentFormat;
   }

   if (inheritance_info->stencilAttachmentFormat) {
      cmdbuf->state.gfx.render.bound_attachments |=
         MESA_VK_RP_ATTACHMENT_STENCIL_BIT;
      cmdbuf->state.gfx.render.s_attachment.fmt =
         inheritance_info->stencilAttachmentFormat;
   }

   const VkRenderingAttachmentLocationInfoKHR att_loc_info_default = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR,
      .colorAttachmentCount = inheritance_info->colorAttachmentCount,
   };
   const VkRenderingAttachmentLocationInfoKHR *att_loc_info =
      vk_get_command_buffer_rendering_attachment_location_info(
         cmdbuf->vk.level, pBeginInfo);
   if (att_loc_info == NULL)
      att_loc_info = &att_loc_info_default;

   vk_cmd_set_rendering_attachment_locations(&cmdbuf->vk, att_loc_info);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdBeginRendering)(VkCommandBuffer commandBuffer,
                                  const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   struct panvk_cmd_graphics_state *state = &cmdbuf->state.gfx;
   bool resuming = pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT;

   /* When resuming from a suspended pass, the state should be unchanged. */
   if (resuming)
      state->render.flags = pRenderingInfo->flags;
   else
      panvk_per_arch(cmd_init_render_state)(cmdbuf, pRenderingInfo);

   /* If we're not resuming, the FBD should be NULL. */
   assert(!state->render.fbds.gpu || resuming);

   if (!resuming)
      panvk_per_arch(cmd_preload_render_area_border)(cmdbuf, pRenderingInfo);
}

static uint8_t
prepare_fb_desc(struct panvk_cmd_buffer *cmdbuf, uint32_t layer, void *fbd)
{
   struct pan_tiler_context tiler_ctx = {
      .valhall.layer_offset = layer - (layer % MAX_LAYERS_PER_TILER_DESC),
   };

   return GENX(pan_emit_fbd)(&cmdbuf->state.gfx.render.fb.info, layer, NULL,
                             &tiler_ctx, fbd);
}

static void
flush_tiling(struct panvk_cmd_buffer *cmdbuf)
{
   if (!cmdbuf->state.gfx.render.fbds.gpu)
      return;

   struct cs_builder *b =
      panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);

   struct cs_index render_ctx = cs_scratch_reg64(b, 2);

   if (cmdbuf->state.gfx.render.tiler) {
      /* Flush the tiling operations and signal the internal sync object. */
      cs_req_res(b, CS_TILER_RES);
      cs_finish_tiling(b, false);
      cs_req_res(b, 0);

      struct cs_index sync_addr = cs_scratch_reg64(b, 0);
      struct cs_index iter_sb = cs_scratch_reg32(b, 2);
      struct cs_index cmp_scratch = cs_scratch_reg32(b, 3);
      struct cs_index add_val = cs_scratch_reg64(b, 4);

      cs_load_to(b, cs_scratch_reg_tuple(b, 0, 3), cs_subqueue_ctx_reg(b),
                 BITFIELD_MASK(3),
                 offsetof(struct panvk_cs_subqueue_context, syncobjs));
      cs_wait_slot(b, SB_ID(LS), false);

      /* We're relying on PANVK_SUBQUEUE_VERTEX_TILER being the first queue to
       * skip an ADD operation on the syncobjs pointer. */
      STATIC_ASSERT(PANVK_SUBQUEUE_VERTEX_TILER == 0);

      cs_move64_to(b, add_val, 1);

      cs_match(b, iter_sb, cmp_scratch) {
#define CASE(x)                                                                \
         cs_case(b, x) {                                                       \
            cs_heap_operation(b,                                               \
                              MALI_CS_HEAP_OPERATION_VERTEX_TILER_COMPLETED,   \
                              cs_defer(SB_WAIT_ITER(x),                        \
                                       SB_ID(DEFERRED_SYNC)));                 \
            cs_sync64_add(b, true, MALI_CS_SYNC_SCOPE_CSG,                     \
                          add_val, sync_addr,                                  \
                          cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC)));    \
            cs_move32_to(b, iter_sb, next_iter_sb(x));                         \
         }

         CASE(0)
         CASE(1)
         CASE(2)
         CASE(3)
         CASE(4)
#undef CASE
      }

      cs_store32(b, iter_sb, cs_subqueue_ctx_reg(b),
                 offsetof(struct panvk_cs_subqueue_context, iter_sb));
      cs_wait_slot(b, SB_ID(LS), false);

      /* Update the vertex seqno. */
      ++cmdbuf->state.cs[PANVK_SUBQUEUE_VERTEX_TILER].relative_sync_point;
   } else {
      cs_load64_to(b, render_ctx, cs_subqueue_ctx_reg(b),
                   offsetof(struct panvk_cs_subqueue_context, render));
      cs_wait_slot(b, SB_ID(LS), false);
   }
}

static void
wait_finish_tiling(struct panvk_cmd_buffer *cmdbuf)
{
   if (!cmdbuf->state.gfx.render.tiler)
      return;

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_FRAGMENT);
   struct cs_index vt_sync_addr = cs_scratch_reg64(b, 0);
   struct cs_index vt_sync_point = cs_scratch_reg64(b, 2);
   uint64_t rel_vt_sync_point =
      cmdbuf->state.cs[PANVK_SUBQUEUE_VERTEX_TILER].relative_sync_point;

   cs_load64_to(b, vt_sync_addr, cs_subqueue_ctx_reg(b),
                offsetof(struct panvk_cs_subqueue_context, syncobjs));
   cs_wait_slot(b, SB_ID(LS), false);

   cs_add64(b, vt_sync_point,
            cs_progress_seqno_reg(b, PANVK_SUBQUEUE_VERTEX_TILER),
            rel_vt_sync_point);
   cs_sync64_wait(b, false, MALI_CS_CONDITION_GREATER, vt_sync_point,
                  vt_sync_addr);
}

static VkResult
issue_fragment_jobs(struct panvk_cmd_buffer *cmdbuf)
{
   if (!cmdbuf->state.gfx.render.fbds.gpu)
      return VK_SUCCESS;

   struct panvk_device *dev = to_panvk_device(cmdbuf->vk.base.device);
   struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.render.fb.info;
   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_FRAGMENT);

   /* Wait for the tiling to be done before submitting the fragment job. */
   wait_finish_tiling(cmdbuf);

   /* Reserve a scoreboard for the fragment job. */
   panvk_per_arch(cs_pick_iter_sb)(cmdbuf, PANVK_SUBQUEUE_FRAGMENT);

   /* Now initialize the fragment bits. */
   cs_update_frag_ctx(b) {
      cs_move32_to(b, cs_sr_reg32(b, 42),
                   (fbinfo->extent.miny << 16) | fbinfo->extent.minx);
      cs_move32_to(b, cs_sr_reg32(b, 43),
                   (fbinfo->extent.maxy << 16) | fbinfo->extent.maxx);
   }

   fbinfo->sample_positions =
      dev->sample_positions->addr.dev +
      panfrost_sample_positions_offset(pan_sample_pattern(fbinfo->nr_samples));

   bool simul_use =
      cmdbuf->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

   /* The only bit we patch in FBDs is the tiler pointer. If tiler is not
    * involved (clear job) or if the update can happen in place (not
    * simultaneous use of the command buffer), we can avoid the
    * copy. */
   bool copy_fbds = simul_use && cmdbuf->state.gfx.render.tiler;
   uint32_t fbd_sz = calc_fbd_size(cmdbuf);
   struct panfrost_ptr fbds = cmdbuf->state.gfx.render.fbds;
   uint8_t fbd_flags = 0;

   VkResult result = panvk_per_arch(cmd_fb_preload)(cmdbuf);
   if (result != VK_SUCCESS)
      return result;

   /* We prepare all FB descriptors upfront. */
   for (uint32_t i = 0; i < cmdbuf->state.gfx.render.layer_count; i++) {
      uint32_t new_fbd_flags =
         prepare_fb_desc(cmdbuf, i, fbds.cpu + (fbd_sz * i));

      /* Make sure all FBDs have the same flags. */
      assert(i == 0 || new_fbd_flags == fbd_flags);
      fbd_flags = new_fbd_flags;
   }

   struct cs_index layer_count = cs_sr_reg32(b, 47);
   struct cs_index fbd_ptr = cs_sr_reg64(b, 48);
   struct cs_index tiler_ptr = cs_sr_reg64(b, 50);
   struct cs_index cur_tiler = cs_sr_reg64(b, 52);
   struct cs_index remaining_layers_in_td = cs_sr_reg32(b, 54);
   struct cs_index src_fbd_ptr = cs_sr_reg64(b, 56);
   uint32_t td_count = DIV_ROUND_UP(cmdbuf->state.gfx.render.layer_count,
                                    MAX_LAYERS_PER_TILER_DESC);

   if (copy_fbds) {
      cs_load64_to(
         b, tiler_ptr, cs_subqueue_ctx_reg(b),
         offsetof(struct panvk_cs_subqueue_context, render.desc_ringbuf.ptr));
      cs_wait_slot(b, SB_ID(LS), false);

      cs_add64(b, fbd_ptr, tiler_ptr, pan_size(TILER_CONTEXT) * td_count);
      cs_move64_to(b, src_fbd_ptr, fbds.gpu);
   } else {
      cs_move64_to(b, fbd_ptr, fbds.gpu);
      if (cmdbuf->state.gfx.render.tiler)
         cs_move64_to(b, tiler_ptr, cmdbuf->state.gfx.render.tiler);
   }


   if (cmdbuf->state.gfx.render.tiler) {
      cs_add64(b, cur_tiler, tiler_ptr, 0);
      cs_move32_to(b, remaining_layers_in_td, MAX_LAYERS_PER_TILER_DESC);
   }

   cs_move32_to(b, layer_count, cmdbuf->state.gfx.render.layer_count);

   cs_req_res(b, CS_FRAG_RES);
   cs_while(b, MALI_CS_CONDITION_GREATER, layer_count) {
      if (copy_fbds) {
         for (uint32_t fbd_off = 0; fbd_off < fbd_sz; fbd_off += 64) {
            cs_load_to(b, cs_scratch_reg_tuple(b, 0, 16), src_fbd_ptr,
                       BITFIELD_MASK(16), fbd_off);
            cs_wait_slot(b, SB_ID(LS), false);
            cs_store(b, cs_scratch_reg_tuple(b, 0, 16), fbd_ptr,
                     BITFIELD_MASK(16), fbd_off);
            cs_wait_slot(b, SB_ID(LS), false);
         }

         cs_add64(b, src_fbd_ptr, src_fbd_ptr, fbd_sz);
      }

      if (cmdbuf->state.gfx.render.tiler) {
         cs_store64(b, cur_tiler, fbd_ptr, 56);
         cs_wait_slot(b, SB_ID(LS), false);
      }

      cs_update_frag_ctx(b)
         cs_add64(b, cs_sr_reg64(b, 40), fbd_ptr, fbd_flags);

      cs_run_fragment(b, false, MALI_TILE_RENDER_ORDER_Z_ORDER, false);
      cs_add64(b, fbd_ptr, fbd_ptr, fbd_sz);
      cs_add32(b, layer_count, layer_count, -1);
      if (cmdbuf->state.gfx.render.tiler) {
         cs_add32(b, remaining_layers_in_td, remaining_layers_in_td, -1);
         cs_if(b, MALI_CS_CONDITION_LEQUAL, remaining_layers_in_td) {
            cs_add64(b, cur_tiler, cur_tiler, pan_size(TILER_CONTEXT));
            cs_move32_to(b, remaining_layers_in_td, MAX_LAYERS_PER_TILER_DESC);
         }
      }
   }
   cs_req_res(b, 0);

   struct cs_index sync_addr = cs_scratch_reg64(b, 0);
   struct cs_index iter_sb = cs_scratch_reg32(b, 2);
   struct cs_index cmp_scratch = cs_scratch_reg32(b, 3);
   struct cs_index add_val = cs_scratch_reg64(b, 4);
   struct cs_index release_sz = cs_scratch_reg32(b, 5);
   struct cs_index ringbuf_sync_addr = cs_scratch_reg64(b, 6);
   struct cs_index completed = cs_scratch_reg_tuple(b, 10, 4);
   struct cs_index completed_top = cs_scratch_reg64(b, 10);
   struct cs_index completed_bottom = cs_scratch_reg64(b, 12);
   struct cs_index tiler_count = cs_sr_reg32(b, 47);

   cs_move64_to(b, add_val, 1);
   cs_load_to(b, cs_scratch_reg_tuple(b, 0, 3), cs_subqueue_ctx_reg(b),
              BITFIELD_MASK(3),
              offsetof(struct panvk_cs_subqueue_context, syncobjs));

   if (copy_fbds) {
      cs_move32_to(b, release_sz, calc_render_descs_size(cmdbuf));
      cs_load64_to(b, ringbuf_sync_addr, cs_subqueue_ctx_reg(b),
                   offsetof(struct panvk_cs_subqueue_context,
                            render.desc_ringbuf.syncobj));
   }

   cs_wait_slot(b, SB_ID(LS), false);

   cs_add64(b, sync_addr, sync_addr,
            PANVK_SUBQUEUE_FRAGMENT * sizeof(struct panvk_cs_sync64));
   cs_move32_to(b, tiler_count, td_count);
   cs_add64(b, cur_tiler, tiler_ptr, 0);

   cs_match(b, iter_sb, cmp_scratch) {
#define CASE(x)                                                                \
      cs_case(b, x) {                                                          \
         if (cmdbuf->state.gfx.render.tiler) {                                 \
            cs_while(b, MALI_CS_CONDITION_GREATER, tiler_count) {              \
               cs_load_to(b, completed, cur_tiler, BITFIELD_MASK(4), 40);      \
               cs_wait_slot(b, SB_ID(LS), false);                              \
               cs_finish_fragment(b, true, completed_top, completed_bottom,    \
                                  cs_defer(SB_WAIT_ITER(x),                    \
                                           SB_ID(DEFERRED_SYNC)));             \
               cs_add64(b, cur_tiler, cur_tiler, pan_size(TILER_CONTEXT));     \
               cs_add32(b, tiler_count, tiler_count, -1);                      \
            }                                                                  \
         }                                                                     \
         if (copy_fbds) {                                                      \
            cs_sync32_add(b, true, MALI_CS_SYNC_SCOPE_CSG,                     \
                          release_sz, ringbuf_sync_addr,                       \
                          cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC)));    \
         }                                                                     \
         cs_sync64_add(b, true, MALI_CS_SYNC_SCOPE_CSG,                        \
                       add_val, sync_addr,                                     \
                       cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC)));       \
         cs_move32_to(b, iter_sb, next_iter_sb(x));                            \
      }

      CASE(0)
      CASE(1)
      CASE(2)
      CASE(3)
      CASE(4)
#undef CASE
   }

   cs_store32(b, iter_sb, cs_subqueue_ctx_reg(b),
              offsetof(struct panvk_cs_subqueue_context, iter_sb));
   cs_wait_slot(b, SB_ID(LS), false);

   /* Update the ring buffer position. */
   if (copy_fbds)
      cs_render_desc_ringbuf_move_ptr(b, calc_render_descs_size(cmdbuf));

   /* Update the frag seqno. */
   ++cmdbuf->state.cs[PANVK_SUBQUEUE_FRAGMENT].relative_sync_point;

   memset(&cmdbuf->state.gfx.render.fbds, 0,
          sizeof(cmdbuf->state.gfx.render.fbds));
   cmdbuf->state.gfx.render.tiler = 0;

   return VK_SUCCESS;
}

void
panvk_per_arch(cmd_flush_draws)(struct panvk_cmd_buffer *cmdbuf)
{
   /* If there was no draw queued, we don't need to force a preload. */
   if (!cmdbuf->state.gfx.render.fbds.gpu)
      return;

   flush_tiling(cmdbuf);
   issue_fragment_jobs(cmdbuf);
   memset(&cmdbuf->state.gfx.render.fbds, 0,
          sizeof(cmdbuf->state.gfx.render.fbds));
   cmdbuf->state.gfx.render.tiler = 0;

   panvk_per_arch(cmd_force_fb_preload)(cmdbuf, NULL);
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdEndRendering)(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (!(cmdbuf->state.gfx.render.flags & VK_RENDERING_SUSPENDING_BIT)) {
      struct pan_fb_info *fbinfo = &cmdbuf->state.gfx.render.fb.info;
      bool clear = fbinfo->zs.clear.z | fbinfo->zs.clear.s;
      for (unsigned i = 0; i < fbinfo->rt_count; i++)
         clear |= fbinfo->rts[i].clear;

      if (clear) {
         VkResult result = get_fb_descs(cmdbuf);
         if (result != VK_SUCCESS)
            return;
      }

      flush_tiling(cmdbuf);
      issue_fragment_jobs(cmdbuf);
      panvk_per_arch(cmd_resolve_attachments)(cmdbuf);
   }
}
