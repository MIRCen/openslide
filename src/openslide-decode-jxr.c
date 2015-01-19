/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
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

#include <config.h>

#ifdef HAVE_LIBJXR

//#include <string.h>
#include "openslide-private.h"
#include "openslide-decode-jxr.h"
#define INITGUID 1
#include <JXRGlue.h>

bool _openslide_jxr_decode_buffer(uint32_t *dest,
                                  int32_t w, int32_t h,
                                  void *data, int32_t datalen,
                                  GError **err) {

  g_debug("_openslide_jxr_decode_buffer:: HAVE_LIBJXR");

  struct WMPStream* pStream = NULL;
  PKImageDecode* pDecoder = NULL;

  // Create stream using input buffer
  Call(CreateWS_Memory(&pStream, data, datalen));

  // Create decoder

  Call(PKImageDecode_Create_WMP(&pDecoder));

  // Attach stream to decoder
  Call(pDecoder->Initialize(pDecoder, pStream));
  // pDecoder->fStreamOwner = !0;

  pDecoder->WMP.wmiI.cfColorFormat = CF_RGB;
  pDecoder->guidPixFormat = GUID_PKPixelFormat24bppBGR;
  pDecoder->WMP.wmiI.bRGB = 1; //RGB

Cleanup:
  return true;
}

#else

bool _openslide_jxr_decode_buffer(uint32_t *dest,
                                  int32_t w, int32_t h,
                                  void *data, int32_t datalen,
                                  GError **err) {
  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Openslide is not able yet to decode JPEG XR" );

  return false;
}

#endif // HAVE_LIBJXR
