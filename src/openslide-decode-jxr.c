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
#define INITGUID 1

#include "openslide-private.h"
#include "openslide-decode-jxr.h"

#ifdef HAVE_LIBJXR

static ERR PKImageEncode_WritePixels_OpenSlide(PKImageEncode* pIE,
                                               U32 cLine,
                                               U8* pbPixel,
                                               U32 cbStride);

static ERR PKImageEncode_Create_OpenSlide(PKImageEncode** ppIE);

static ERR ResetWS_Memory(struct WMPStream** ppWS, void* pv);

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

// Reset an allocated WMPStream using a new data pointer without 
// reallocating the internal buffer, without emptying data.
ERR ResetWS_Memory(struct WMPStream** ppWS, void* pv)
{
    ERR err = WMP_errSuccess;
    struct WMPStream* pWS = *ppWS;

    pWS->state.buf.pbBuf = pv;
    pWS->state.buf.cbCur = 0;

    return err;
}

#endif

static bool _openslide_jxr_decoder_initialize(struct jxr_decoder * decoder,
                                              uint32_t datalen,
                                              int32_t w, int32_t h,
                                              GError **error) {
#ifdef HAVE_LIBJXR // HAVE_LIBJXR
    
  // g_debug("_openslide_jxr_decode_buffer:: HAVE_LIBJXR");

  g_assert(decoder);
  
  decoder->pStream = NULL;
  decoder->pEncodeStream = NULL;
  decoder->pDecoder = NULL;
  decoder->pEncoder = NULL;
  decoder->pConverter = NULL;
  ERR err = WMP_errSuccess;

  // Get information on
  const PKPixelFormatGUID * pxfg = &GUID_PKPixelFormat24bppBGR;

  decoder->PI.pGUIDPixFmt = pxfg;
  PixelFormatLookup(&(decoder->PI), LOOKUP_FORWARD);

  // Create a converter
  Call(PKCodecFactory_CreateFormatConverter(&(decoder->pConverter)));

  // Set decoding region
  decoder->region = (PKRect){0, 0, 0, 0};
  decoder->region.Width = (I32)w;
  decoder->region.Height = (I32)h;

  // Create stream using input/output buffer
  Call(CreateWS_Memory(&(decoder->pStream), NULL, datalen));
  Call(CreateWS_Memory(&(decoder->pEncodeStream), NULL, w * h * 3));

  // Create decoder/encoder
  Call(PKImageDecode_Create_WMP(&(decoder->pDecoder)));
  Call(PKImageEncode_Create_OpenSlide(&(decoder->pEncoder)));

  // Set decoder options
  decoder->pDecoder->WMP.wmiI.cfColorFormat = decoder->PI.cfColorFormat;
  decoder->pDecoder->guidPixFormat = *(decoder->PI.pGUIDPixFmt);
  decoder->pDecoder->WMP.wmiI.cfColorFormat = decoder->PI.cfColorFormat;
  decoder->pDecoder->WMP.wmiI.bdBitDepth = decoder->PI.bdBitDepth;
  decoder->pDecoder->WMP.wmiI.cBitsPerUnit = decoder->PI.cbitUnit;
  decoder->pDecoder->WMP.wmiI.cROIWidth = w;
  decoder->pDecoder->WMP.wmiI.cROIHeight = h;
  decoder->pDecoder->WMP.wmiSCP.uAlphaMode = 0;
  decoder->pDecoder->WMP.wmiSCP.sbSubband = SB_ALL;
  decoder->pDecoder->WMP.bIgnoreOverlap = FALSE;
  decoder->pDecoder->WMP.wmiI.cThumbnailWidth = decoder->pDecoder->WMP.wmiI.cWidth;
  decoder->pDecoder->WMP.wmiI.cThumbnailHeight = decoder->pDecoder->WMP.wmiI.cHeight;
  decoder->pDecoder->WMP.wmiI.bSkipFlexbits = FALSE;

  // Set encoder write source method
  decoder->pEncoder->WriteSource = PKImageEncode_Transcode;
    
  Call(
    decoder->pConverter->Initialize(
      decoder->pConverter, decoder->pDecoder,
        NULL, *(decoder->PI.pGUIDPixFmt)
    ));
  
  Call(
    decoder->pEncoder->SetSize(
      decoder->pEncoder, 
      decoder->region.Width,
      decoder->region.Height));
  
  decoder->initialized = true;
  return true;

Cleanup:
  // Release objects
  decoder->finalize(decoder, error);

  if (err != 0) {
    g_debug("Error %ld occured while uncompressing using JPEGXR (initilization)", err);
    g_set_error( error, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    "Error %ld occured while uncompressing using JPEGXR", err );
  }

#endif
  
  return false;
}

static bool _openslide_jxr_decoder_decode(struct jxr_decoder * decoder,
                                          const void *data,
                                          uint32_t *dest,
                                          GError **error) {
#ifdef HAVE_LIBJXR
  g_assert(decoder);
  
  ERR err = WMP_errSuccess;
  
  // Reset streams using input/output buffers
  ResetWS_Memory(&(decoder->pStream), (void *)data);
  ResetWS_Memory(&(decoder->pEncodeStream), (void *)dest);
  
  // Attach stream to decoder/encoder
  // It is necessary to do decoder/encoder with the correct buffers
  Call(
    decoder->pDecoder->Initialize(
      decoder->pDecoder, decoder->pStream
    ));
  
  Call(
    decoder->pEncoder->Initialize(
      decoder->pEncoder, decoder->pEncodeStream, NULL, 0
    ));
  
  Call(
    decoder->pEncoder->SetPixelFormat(
      decoder->pEncoder, 
      *(decoder->PI.pGUIDPixFmt)));

  // Convert data from original image
  Call(
    decoder->pEncoder->WriteSource(
      decoder->pEncoder, 
      decoder->pConverter, 
      &(decoder->region)
    ));
  
  return true;
  
Cleanup:
  // Release objects
  decoder->finalize(decoder, error);

  if (err != 0) {
    g_debug("Error %ld occured while uncompressing using JPEGXR (decode)", err);
    g_set_error( error, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
    "Error %ld occured while uncompressing using JPEGXR", err );
  }
  
#endif // HAVE_LIBJXR

  return false;
}

bool _openslide_jxr_decoder_finalize(struct jxr_decoder * decoder,
                                     GError            ** error G_GNUC_UNUSED) {
#ifdef HAVE_LIBJXR
  if(decoder) {
    // Release objects
    if (decoder->pEncoder)
        decoder->pEncoder->Release(&(decoder->pEncoder));
    
    if (decoder->pDecoder)
        decoder->pDecoder->Release(&(decoder->pDecoder));
    
    if (decoder->pConverter)
        decoder->pConverter->Release(&(decoder->pConverter));
    
    decoder->initialized = false;
    
    return true;
  }
  
#endif // HAVE_LIBJXR

  return false;
}

struct jxr_decoder * openslide_jxr_decoder_new(GError ** error G_GNUC_UNUSED)
{
  struct jxr_decoder * ptr = (struct jxr_decoder*) 
                                 g_slice_alloc0(
                                   sizeof(struct jxr_decoder)
                                 );
  ptr->initialize = _openslide_jxr_decoder_initialize;
  ptr->decode = _openslide_jxr_decoder_decode;
  ptr->finalize = _openslide_jxr_decoder_finalize;
  ptr->initialized = false;
  
  return ptr;
}


bool openslide_jxr_decoder_free(struct jxr_decoder * ptr, GError ** error)
{
  if (ptr) {
      
    ptr->finalize(ptr, error);
    g_slice_free(struct jxr_decoder, ptr);
    
    return true;
  }
  
  return false;
}

bool _openslide_jxr_decode_buffer(const void *data,
                                  uint32_t datalen,
                                  uint32_t *dest,
                                  int32_t w,
                                  int32_t h,
                                  GError **error) {
#ifdef HAVE_LIBJXR
  
  //  JpegXr plugin today only support 24 bit BGR (3 x 8 bit) color 
  //  TODO: Add support for float (32 bit), 24 bit (3x 16 bit) color,
  //       8 bit and 16 bit greyscale
    
  struct jxr_decoder * os_jxr_decoder = openslide_jxr_decoder_new(error);
  os_jxr_decoder->initialize(os_jxr_decoder, datalen, w, h, error);
  os_jxr_decoder->decode(os_jxr_decoder, data, dest, error);
  os_jxr_decoder->finalize(os_jxr_decoder, error);
  openslide_jxr_decoder_free(os_jxr_decoder, error);
  
  return true;

#else // HAVE_LIBJXR

  g_set_error( error, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Openslide is not able to decode JPEG XR" );

  return false;

#endif // HAVE_LIBJXR

}
