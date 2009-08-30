/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2007,2008,2009 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __COGL_TEXTURE_2D_SLICED_H
#define __COGL_TEXTURE_2D_SLICED_H

#include "cogl-bitmap-private.h"
#include "cogl-handle.h"
#include "cogl-material-private.h"
#include "cogl-texture-private.h"

#define COGL_TEXTURE_2D_SLICED(tex) ((CoglTexture2DSliced *)tex)

typedef struct _CoglTexture2DSliced CoglTexture2DSliced;
typedef struct _CoglTexSliceSpan  CoglTexSliceSpan;
typedef struct _CoglSpanIter      CoglSpanIter;
typedef struct _CoglTexturePixel  CoglTexturePixel;

struct _CoglTexSliceSpan
{
  int start;
  int size;
  int waste;
};

struct _CoglSpanIter
{
  gint              index;
  GArray           *array;
  CoglTexSliceSpan *span;
  float             pos;
  float             next_pos;
  float             origin;
  float             cover_start;
  float             cover_end;
  float             intersect_start;
  float             intersect_end;
  float             intersect_start_local;
  float             intersect_end_local;
  gboolean          intersects;
  gboolean          flipped;
};

/* This is used to store the first pixel of each slice. This is only
   used when glGenerateMipmap is not available */
struct _CoglTexturePixel
{
  /* We need to store the format of the pixel because we store the
     data in the source format which might end up being different for
     each slice if a subregion is updated with a different format */
  GLenum gl_format;
  GLenum gl_type;
  guint8 data[4];
};

struct _CoglTexture2DSliced
{
  CoglTexture       _parent;
  GArray            *slice_x_spans;
  GArray            *slice_y_spans;
  GArray            *slice_gl_handles;
  gint               max_waste;

  /* This holds a copy of the first pixel in each slice. It is only
     used to force an automatic update of the mipmaps when
     glGenerateMipmap is not available. */
  CoglTexturePixel  *first_pixels;
};

GQuark
_cogl_handle_texture_2d_sliced_get_type (void);

CoglHandle
_cogl_texture_2d_sliced_new_with_size (unsigned int     width,
                                       unsigned int     height,
                                       CoglTextureFlags flags,
                                       CoglPixelFormat  internal_format);

CoglHandle
_cogl_texture_2d_sliced_new_from_file (const gchar       *filename,
                                       CoglTextureFlags   flags,
                                       CoglPixelFormat    internal_format,
                                       GError           **error);

CoglHandle
_cogl_texture_2d_sliced_new_from_foreign (GLuint           gl_handle,
                                          GLenum           gl_target,
                                          GLuint           width,
                                          GLuint           height,
                                          GLuint           x_pot_waste,
                                          GLuint           y_pot_waste,
                                          CoglPixelFormat  format);

CoglHandle
_cogl_texture_2d_sliced_new_from_data (unsigned int     width,
                                       unsigned int     height,
                                       CoglTextureFlags flags,
                                       CoglPixelFormat  format,
                                       CoglPixelFormat  internal_format,
                                       unsigned int     rowstride,
                                       const guint8    *data);


CoglHandle
_cogl_texture_2d_sliced_new_from_bitmap (CoglHandle       bmp_handle,
                                         CoglTextureFlags flags,
                                         CoglPixelFormat  internal_format);

gboolean
_cogl_texture_2d_sliced_set_region (CoglHandle      handle,
                                    int             src_x,
                                    int             src_y,
                                    int             dst_x,
                                    int             dst_y,
                                    unsigned int    dst_width,
                                    unsigned int    dst_height,
                                    int             width,
                                    int             height,
                                    CoglPixelFormat format,
                                    unsigned int    rowstride,
                                    const guint8   *data);

int
_cogl_texture_2d_sliced_get_data (CoglHandle       handle,
                                  CoglPixelFormat  format,
                                  unsigned int     rowstride,
                                  guint8          *data);

void
_cogl_texture_2d_sliced_foreach_sub_texture_in_region (
                                            CoglTexture2DSliced *tex_2ds,
                                            float virtual_tx_1,
                                            float virtual_ty_1,
                                            float virtual_tx_2,
                                            float virtual_ty_2,
                                            CoglTextureSliceCallback callback,
                                            void *user_data);

gint
_cogl_texture_2d_sliced_get_max_waste (CoglHandle handle);

gboolean
_cogl_texture_2d_sliced_is_sliced (CoglHandle handle);

gboolean
_cogl_texture_2d_sliced_can_hardware_repeat (CoglHandle handle);

void
_cogl_texture_2d_sliced_transform_coords_to_gl (CoglTexture2DSliced *tex_2ds,
                                                float *s,
                                                float *t);

gboolean
_cogl_texture_2d_sliced_get_gl_texture (CoglHandle handle,
			                GLuint *out_gl_handle,
			                GLenum *out_gl_target);

void
_cogl_texture_2d_sliced_set_filters (CoglHandle handle,
                                     GLenum min_filter,
                                     GLenum mag_filter);

void
_cogl_texture_2d_sliced_ensure_mipmaps (CoglHandle handle);

void
_cogl_texture_2d_sliced_set_wrap_mode_parameter (CoglTexture2DSliced *tex_2ds,
                                                 GLenum wrap_mode);


#endif /* __COGL_TEXTURE_2D_SLICED_H */