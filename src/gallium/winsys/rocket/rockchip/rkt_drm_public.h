/*
 * Copyright 2014 Broadcom
 * Copyright 2018 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#ifndef __RKT_DRM_PUBLIC_H__
#define __RKT_DRM_PUBLIC_H__

#include <stdbool.h>

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

struct pipe_screen *
rkt_drm_screen_create(int drmFD, const struct pipe_screen_config *config);
struct pipe_screen *
rkt_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                   const struct pipe_screen_config *config);

#endif /* __RKT_DRM_PUBLIC_H__ */
