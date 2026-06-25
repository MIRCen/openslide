/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifndef OPENSLIDE_OPENSLIDE_DECODE_JXR_H_
#define OPENSLIDE_OPENSLIDE_DECODE_JXR_H_


#include <stdint.h>
#include <glib.h>

#include <config.h>

/* JPEG XR support */
#ifdef HAVE_LIBJXR
#include <JXRGlue.h>
#endif

struct jxr_decoder {
    
#ifdef HAVE_LIBJXR
  struct WMPStream* pStream;
  struct WMPStream* pEncodeStream;
  
  PKImageDecode* pDecoder;
  PKImageEncode* pEncoder;
  PKFormatConverter* pConverter;
  
  PKPixelInfo PI;
  PKRect region;
  
#endif // HAVE_LIBJXR
  
  bool initialized;
  
  bool (*initialize)(struct jxr_decoder * decoder,
                     uint32_t datalen,
                     int32_t w, int32_t h,
                     int32_t pixeltype,
                     GError **error);
  bool (*decode)(struct jxr_decoder * decoder,
                 const void *data,
                 uint32_t *dest,
                 GError **error);
  bool (*finalize)(struct jxr_decoder * decoder,
                   GError **error);
};


enum pixel_type {
  JXR_PT_GRAY8 = 0,
  JXR_PT_GRAY16 = 1,
  // JXR_PT_GRAY32FLOAT = 2,
  JXR_PT_BGR24 = 3,
  // JXR_PT_BGR48 = 4,
  // JXR_PT_BGR96FLOAT = 8,
  // JXR_PT_BGRA32,
  // JXR_PT_GRAY64COMPLEX,
  // JXR_PT_BGR192COMPLEX,
  // JXR_PT_GRAY32,
  // JXR_PT_GRAY64,
};

uint32_t PixelType_GetSize(int32_t pixeltype);

struct jxr_decoder * openslide_jxr_decoder_new(GError ** error);

bool _openslide_jxr_decoder_finalize(struct jxr_decoder * decoder,
                                     GError            ** error);

bool openslide_jxr_decoder_free(struct jxr_decoder * ptr,
                                GError            ** error);

bool _openslide_jxr_decode_buffer(const void       * data,
                                  uint32_t           datalen,
                                  uint32_t         * dest,
                                  int32_t            w,
                                  int32_t            h,
                                  int32_t            pixeltype,
                                  GError          ** err);

void *_openslide_jxr_decompress_buffer(const void * data,
                                       uint32_t     datalen,
                                       int32_t      w,
                                       int32_t      h,
                                       int32_t      pixeltype,
                                       GError    ** error);
#endif
