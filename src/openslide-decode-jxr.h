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
                     GError **error);
  bool (*decode)(struct jxr_decoder * decoder,
                 const void *data,
                 uint32_t *dest,
                 GError **error);
  bool (*finalize)(struct jxr_decoder * decoder,
                   GError **error);
};


struct jxr_decoder * openslide_jxr_decoder_new(GError ** error G_GNUC_UNUSED);

bool openslide_jxr_decoder_free(struct jxr_decoder * ptr,
                                GError            ** error  G_GNUC_UNUSED);

bool _openslide_jxr_decode_buffer(const void       * data    G_GNUC_UNUSED,
                                  uint32_t           datalen G_GNUC_UNUSED,
                                  uint32_t         * dest    G_GNUC_UNUSED,
                                  int32_t            w       G_GNUC_UNUSED,
                                  int32_t            h       G_GNUC_UNUSED,
                                  GError          ** err     G_GNUC_UNUSED);

#endif
