/*
 * ZEISS (czi) zisraw support
 * Yaël
 *
 * In this file we have all structures and methods used to understand 
 * the zisraw format.
 * At first I don't follow openslide rules (Gerror etc). It will be time to 
 * do this later.
 */

#ifndef OPENSLIDE_OPENSLIDE_DECODE_ZISRAW_H_
#define OPENSLIDE_OPENSLIDE_DECODE_ZISRAW_H_

//--- std --------------------------------------------------------------------
#include <glib.h>                                                // GError
#include <stdint.h>                                              // intX_t
#include <stdbool.h>                                             // bool
//----------------------------------------------------------------------------

//============================================================================
//
//                            PRIVATE STRUCTURES
//
//============================================================================

// ===========================================================================
//    TYPES 
// ===========================================================================

enum czi_pixel_t {
  PXL_UNKNOWN            = -1,
  GRAY_8                 = 0,
  GRAY_16                = 1,
  GRAY_32_FLOAT          = 2,
  BGR_24                 = 3,
  BGR_48                 = 4,
  BGR_96_FLOAT           = 8,
  BGRA_32                = 9,
  GRAY_64_COMPLEX_FLOAT  = 10,
  BGR_192_COMPLEX_FLOAT  = 11,
  GRAY_32                = 12,
  GRAY_64                = 13
};

enum czi_compression_t {
  CMP_UNKNOWN            = -1,
  UNCOMPRESSED           = 0,
  JPEG                   = 1,
  LZW                    = 2,
  JPEGXR                 = 4,
  CAMERA_SPEC            = 100,
  SYSTEM_SPEC            = 1000,
};

enum czi_pyramid_t {
  PYR_UNKNOWN            = -1,
  NONE                   = 0,
  SINGLE                 = 1,
  MULTI                  = 2
};

// ===========================================================================
//    API STRUCTURES
// ===========================================================================

// Descriptive structure used to navigate in the czi file.
// Its characteristics should be hidden to the user.
typedef struct _czi _openslide_czi;

// Tile descriptive structure
// Made for the user to choose tiles to load.
// Loading should be done using appropriate function
struct _openslide_czi_tile_descriptor {
  uint8_t                 guid[16];       // unique identifier to access tiles
  enum czi_pixel_t        pixel_type;     // pixel type on disk
  enum czi_compression_t  compression;    // data compression
  enum czi_pyramid_t      pyramid_type;   // subsampling type (none - one dir - two dir)
  int32_t                 subsampling_x;  // number of pixels aggregated in x direction
  int32_t                 subsampling_y;  // number of pixels aggregated in y direction
  int32_t                 start_x;        // position of top-left pixel on the grid (pyr 0 referential)
  int32_t                 start_y;        // position of top-left pixel on the grid (pyr 0 referential)
  int32_t                 size_x;         // size of the tile (pyr 0 referential)
  int32_t                 size_y;         // size of the tile (pyr 0 referential)
};

// ===========================================================================
//    API METHODS
// ===========================================================================

// Looks for ZISRAW magic string
bool _openslide_czi_is_zisraw( const char * filename, GError ** err );

// Setting of _openslide_czi structure
// Everything is read except data blocks (tiles data, xml blocks, attachments data)
_openslide_czi * _openslide_czi_decode( const char * filename, GError ** err );

// A few precomputed characteristics to detect unsupported files
bool _openslide_czi_is_multi_view( _openslide_czi * czi );
bool _openslide_czi_is_multi_phase( _openslide_czi * czi );
bool _openslide_czi_is_multi_block( _openslide_czi * czi );
bool _openslide_czi_is_multi_illumination( _openslide_czi * czi );
bool _openslide_czi_is_multi_rotation( _openslide_czi * czi );
bool _openslide_czi_is_multi_time( _openslide_czi * czi );
bool _openslide_czi_is_multi_zslice( _openslide_czi * czi );
bool _openslide_czi_is_multi_channel( _openslide_czi * czi );
bool _openslide_czi_has_data_uncompressed( _openslide_czi * czi );
bool _openslide_czi_has_data_jpg( _openslide_czi * czi );
bool _openslide_czi_has_data_jpgxr( _openslide_czi * czi );
bool _openslide_czi_has_data_lzw( _openslide_czi * czi );
bool _openslide_czi_has_data_cameraspec( _openslide_czi * czi );
bool _openslide_czi_has_data_systemspec( _openslide_czi * czi );

// Tiles
int32_t   _openslide_czi_get_level_count( _openslide_czi * czi, GError **err );
GList   * _openslide_czi_get_level_tiles( _openslide_czi * czi, GError **err );
uint8_t * _openslide_czi_load_tile( _openslide_czi * czi, char * guid, GError **err );
void      _openslide_czi_destroy_tile( _openslide_czi * czi, char * guid, GError **err );

// Metadata
// There is one metadata block per file. In the multi-file case, I guess 
// the same metadata block is stored in each file.
// Still, we give the possibility to choose which metadata block to load.
// In all cases at least one metadata block is present, so calling the 
// load method with index 0 should always return something.
int32_t   _openslide_czi_get_metadata_count( _openslide_czi * czi, GError **err );
uint8_t * _openslide_czi_load_metadata( _openslide_czi * czi, int32_t index, GError **err );
void      _openslide_czi_destroy_metadata( _openslide_czi * czi, int32_t index ); // Or have the user do it ?

// Attachments
// If a null pointer is returned along with no error, it means that the 
// attachment is not stored in the file
_openslide_czi * _openslide_czi_decode_label( _openslide_czi * czi, GError **err );
_openslide_czi * _openslide_czi_decode_prescan( _openslide_czi * czi, GError **err );
_openslide_czi * _openslide_czi_decode_slide_preview( _openslide_czi * czi, GError **err );

// Free
// We give functions to free -openslide_czi structure,
// GList[ _openslide_czi_tile_descriptor ] and any loaded buffer.
// Buffers must be destroyed right after they are used : Freeing 
// _openslide_czi won't destroy thoses buffers !
void _openslide_czi_free( _openslide_czi * czi );

#endif