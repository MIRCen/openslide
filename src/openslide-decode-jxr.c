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

#include "openslide-private.h"
#include "openslide-decode-jxr.h"
#define INITGUID 1
#include <JXRGlue.h>

static ERR PKImageEncode_WritePixels_OpenSlide( PKImageEncode* pIE,
                                                U32 cLine,
                                                U8* pbPixel,
                                                U32 cbStride);

static ERR PKImageEncode_Create_OpenSlide(PKImageEncode** ppIE);

//================================================================
// PKImageEncode_OpenSlide
//================================================================
ERR PKImageEncode_WritePixels_OpenSlide(
    PKImageEncode* pIE,
    U32 cLine,
    U8* pbPixel,
    U32 cbStride)
{
    ERR err = WMP_errSuccess;

    struct WMPStream* pS = pIE->pStream;
    PKPixelInfo PI;
    size_t cbLine = 0;
    size_t offPos = 0;
    size_t i = 0;

    // Get encoding pixel format informations
    PI.pGUIDPixFmt = &pIE->guidPixFormat;
    PixelFormatLookup(&PI, LOOKUP_FORWARD);

    // Number of bytes per line in output image
    cbLine = (BD_1 == PI.bdBitDepth ?
              ((PI.cbitUnit * pIE->uWidth + 7) >> 3)
            : (((PI.cbitUnit + 7) >> 3) * pIE->uWidth));

    FailIf(cbStride < cbLine, WMP_errInvalidParameter);
    offPos = pIE->offPixel + cbLine * pIE->idxCurrentLine;

    // g_debug( "PKImageEncode_WritePixels_OpenSlide:: cbLine: %ld, "
    //          "uWidth:%d, idxCurrentLine: %d, offPixel: %d, offPos:%ld, "
    //          "bdBitDepth: %d,cbitUnit: %d, cbStride: %u",
    //          cbLine,
    //          pIE->uWidth,
    //          pIE->idxCurrentLine,
    //          pIE->offPixel,
    //          offPos,
    //          PI.bdBitDepth,
    //          PI.cbitUnit,
    //          cbStride );

    Call(pS->SetPos(pS, offPos));

    for (i = 0; i < cLine; ++i)
    {
        Call(pS->Write(pS, pbPixel + cbStride * i, cbLine));
    }
    pIE->idxCurrentLine += cLine;

Cleanup:
    return err;
}

ERR PKImageEncode_Create_OpenSlide(PKImageEncode** ppIE)
{
    ERR err = WMP_errSuccess;

    PKImageEncode* pIE = NULL;

    Call(PKImageEncode_Create(ppIE));

    pIE = *ppIE;
    pIE->WritePixels = PKImageEncode_WritePixels_OpenSlide;

Cleanup:
    return err;
}

#endif // HAVE_LIBJXR


bool _openslide_jxr_decode_buffer(const void *data, uint32_t datalen,
                                   uint32_t *dest,
                                   int32_t w, int32_t h,
                                   GError **error) {
#ifdef HAVE_LIBJXR // HAVE_LIBJXR

  // g_debug("_openslide_jxr_decode_buffer:: HAVE_LIBJXR");

  struct WMPStream* pStream = NULL;
  struct WMPStream* pEncodeStream = NULL;
  PKImageDecode* pDecoder = NULL;
  PKImageEncode* pEncoder = NULL;
  PKFormatConverter* pConverter = NULL;
  ERR err = WMP_errSuccess;

  // Get information on
  const PKPixelFormatGUID * pxfg = &GUID_PKPixelFormat24bppBGR;

  PKPixelInfo PI;
  PI.pGUIDPixFmt = pxfg;
  PixelFormatLookup(&PI, LOOKUP_FORWARD);

  // Create a converter
  Call(PKCodecFactory_CreateFormatConverter(&pConverter));

  // Set decoding region
  PKRect region = {0, 0, 0, 0};
  region.Width = (I32)w;
  region.Height = (I32)h;

  // Create stream using input/output buffer
  Call(CreateWS_Memory(&pStream, (void *)data, datalen));
  Call(CreateWS_Memory(&pEncodeStream, (void *)dest, w * h * 3));

  // Create decoder/encoder
  Call(PKImageDecode_Create_WMP(&pDecoder));
  Call(PKImageEncode_Create_OpenSlide(&pEncoder));

  // Attach stream to decoder/encoder
  Call(pDecoder->Initialize(pDecoder, pStream));
  Call(pEncoder->Initialize(pEncoder, pEncodeStream, NULL, 0));

  // Set decoder options
  pDecoder->WMP.wmiI.cfColorFormat = PI.cfColorFormat;
  pDecoder->guidPixFormat = *PI.pGUIDPixFmt;
  pDecoder->WMP.wmiI.cfColorFormat = PI.cfColorFormat;
  pDecoder->WMP.wmiI.bdBitDepth = PI.bdBitDepth;
  pDecoder->WMP.wmiI.cBitsPerUnit = PI.cbitUnit;
  pDecoder->WMP.wmiI.cROIWidth = w;
  pDecoder->WMP.wmiI.cROIHeight = h;
  pDecoder->WMP.wmiSCP.uAlphaMode = 0;
  pDecoder->WMP.wmiSCP.sbSubband = SB_ALL;
  pDecoder->WMP.bIgnoreOverlap = FALSE;
  pDecoder->WMP.wmiI.cThumbnailWidth = pDecoder->WMP.wmiI.cWidth;
  pDecoder->WMP.wmiI.cThumbnailHeight = pDecoder->WMP.wmiI.cHeight;
  pDecoder->WMP.wmiI.bSkipFlexbits = FALSE;

  Call(pConverter->Initialize(pConverter, pDecoder,
                              NULL, *PI.pGUIDPixFmt));

  // Set encoder write source method
  pEncoder->WriteSource = PKImageEncode_Transcode;

  Call(pEncoder->SetPixelFormat(pEncoder, *PI.pGUIDPixFmt ));
  Call(pEncoder->SetSize(pEncoder, region.Width, region.Height));

  // Convert data from original image
  Call(pEncoder->WriteSource(pEncoder, pConverter, &region));

Cleanup:
  // Release objects
  pEncoder->Release(&pEncoder);
  pDecoder->Release(&pDecoder);
  pConverter->Release(&pConverter);

  if (err != 0) {
    g_debug("Error %ld occured while uncompressing using JPEGXR", err);
    g_set_error( error, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    "Error %ld occured while uncompressing using JPEGXR", err );
    return false;
  }
  else
    return true;

#else // HAVE_LIBJXR

  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Openslide is not able to decode JPEG XR" );

  return false;

#endif // HAVE_LIBJXR

}
