/*
 * ZEISS (czi) zisraw support
 * Yaël
 *
 * CZI is a format composed of a ZISRAW base which is a tiff-like,
 * segment-based structure, and a CZI layer which specifies hardware-specific
 * properties inside xml blocks.
 * For readability purposes, as-well as allowing eventual layer separation,
 * we use an API for ZISRAW parsing which only uses glib and some
 * "openslide-private" functions (fopen...), and a czi part which implements
 * OpenSlide's API (open, debug, readtile, ...) and does the actual data
 * decoding (jpeg, xml, ...).
 * Please only use "public ZISRAW API" functions in the latter (private
 * ZISRAW functions should only be used inside the file parsing layer).
 *
 * You will thus find in this file:                       Ctrl+F key
 *   I)  ZISRAW STRUCTURE PARSING                         key:PARSING-TOP
 *       1) DECLARATIONS
 *          - ZISRAW PUBLIC API DECLARATIONS              key:PARSING-PUB-DECL
 *          - ZISRAW PRIVATE METHODS DECLARATIONS         key:PARSING-PRI-DECL
 *       2) DEFINITIONS
 *          - ZISRAW PRIVATE METHODS DEFINITIONS          key:PARSING-PRI-DEF
 *          - ZISRAW PUBLIC API DEFINITIONS               key:PARSING-PUB-DEF
 *   II) ZEISS-VENDOR DRIVER                              key:DRIVER-TOP
 *       1) DECLARATIONS
 *          - OPENSLIDE API DECLARATIONS                  key:DRIVER-API-DECL
 *          - PRIVATE METHODS DECLARATIONS                key:DRIVER-PRI-DECL
 *       2) DEFINITIONS
 *          - PRIVATE METHODS DEFINITIONS                 key:DRIVER-PRI-DEF
 *          - OPENSLIDE API DEFINITIONS                   key:DRIVER-API-DEF
 *
 * Structure parsing public methods are named      _openslide_czi_*
 * Structure parsing public structures are named   struct _openslide_czi_*
 * Structure parsing public enum are named         enum czi*
 *
 * Structure parsing private methods are named      czi_*
 * Structure parsing private structures are named   struct _czi*
 *
 * Zeiss vendor driver methods are names            zeiss_*
 */

//--- openslide --------------------------------------------------------------
#include "openslide-decode-xml.h"
#include "openslide-decode-jpeg.h"
#ifdef HAVE_LIBJXR
#include "openslide-decode-jxr.h"
#endif //HAVE_LIBJXR
#include "openslide-private.h"
//--- extern -----------------------------------------------------------------
// specify which is needed for ZISRAW ".h", ZISRAW ".c", ZEISS ".c"
#include <glib.h>
#include <math.h>
#include <limits.h>                                      // type limits
#include <complex.h>                                     // complex float data type
#include <stdint.h>                                      // c99 int data types
#include <stdbool.h>                                                   // bool
#include <stdio.h>                                            // file handling
#include <errno.h>                                      // file handling error
#include <string.h>                                       // string comparison
#include <sys/types.h>                                   // int MIN/MAX values
//----------------------------------------------------------------------------

//============================================================================
//   PRIVATE DEFINES
//============================================================================
#define CZI_DISPLAY_INDENT          2
//#define CZI_DEBUG                   1
//#define CZI_WRITE_TILE_DATA         1
//#define CZI_WRITE_XML               1

// key:PARSING-TOP
//////////////////////////////////////////////////////////////////////////////
///                                                                        ///
///             Z I S R A W   S T R U C T U R E   P A R S I N G            ///
///                                                                        ///
//////////////////////////////////////////////////////////////////////////////

// key:PARSING-PUB-DECL
//============================================================================
//
//                ZISRAW PARSING PUBLIC API : DECLARATIONS
//
//============================================================================

// ===========================================================================
//    PUBLIC TYPES
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

enum czi_roi_shape_t {
  SHP_UNKNOWN            = -1,
  RECTANGLE              = 0,
  ELLIPSE                = 1,
  POLYGON                = 2
};


enum czi_roi_covering_mode_t {
  COV_UNKNOWN                  = -1,
  ALIGNED_TO_GLOBAL_GRID       = 0,
  ALIGNED_TO_LOCAL_TILE_REGION = 1
};

// ===========================================================================
//    PUBLIC STRUCTURES
// ===========================================================================

// Descriptive structure used to navigate in the czi file.
// Its characteristics should be hidden to the user.
typedef struct _czi _openslide_czi;
typedef struct _czi_roi _openslide_roi; // Temporary, this is to allow later renaming

// Tile descriptive structure
// Made for the user to choose tiles to load.
// Loading should be done using appropriate function
struct _openslide_czi_tile_descriptor {
  int64_t                 uid;            // unique identifier to access tiles
  enum czi_pixel_t        pixel_type;     // pixel type on disk
  enum czi_compression_t  compression;    // data compression
  enum czi_pyramid_t      pyramid_type;   // subsampling type (none - one dir - two dir)
  int32_t                 subsampling_x;  // number of pixels aggregated in x direction
  int32_t                 subsampling_y;  // number of pixels aggregated in y direction
  int32_t                 start_x;        // position of top-left pixel in global (pyr0) referential
  int32_t                 start_y;        // position of top-left pixel in global (pyr0) referential
  int32_t                 size_x;         // size of the tile (pyr 0 referential)
  int32_t                 size_y;         // size of the tile (pyr 0 referential)
};

// Uncompressor structure
// Made to be able to uncompress data to a destination buffer
struct _openslide_czi_uncompressor {
  char * name;                                            // Name of uncompressor

  bool (*uncompress)( const void *data, uint32_t data_size,
                      uint32_t *dest,
                      int32_t width, int32_t height,
                      GError **err );                     // Uncompress method
};

const struct _openslide_czi_uncompressor _openslide_uncompressor_jpeg = {
  .name   = "jpeg",
  .uncompress = _openslide_jpeg_decode_buffer,
};

#ifdef HAVE_LIBJXR

const struct _openslide_czi_uncompressor _openslide_uncompressor_jxr = {
  .name   = "jpegxr",
  .uncompress = _openslide_jxr_decode_buffer,
};

#endif

// ===========================================================================
//    PUBLIC METHODS
// ===========================================================================

// Uncompress
static uint8_t * _openslide_czi_uncompress( const struct _openslide_czi_uncompressor * uncompressor,
                                          void * data, int32_t data_size,
                                          int32_t width, int32_t height,
                                          enum czi_pixel_t pixel_type,
                                          int32_t * uncompressed_data_size,
                                          GError ** err );

// Looks for ZISRAW magic string
static bool _openslide_czi_is_zisraw( const char * filename, GError ** err );

// Setting of _openslide_czi structure
// Everything is read except data blocks (tiles data, xml blocks, attachments data)
static _openslide_czi * _openslide_czi_decode( const char * filename, GError ** err );
static void _openslide_czi_free( _openslide_czi * czi );

// A few precomputed characteristics to detect unsupported files
static bool _openslide_czi_is_multi_view( _openslide_czi * czi );
static bool _openslide_czi_is_multi_phase( _openslide_czi * czi );
static bool _openslide_czi_is_multi_block( _openslide_czi * czi );
static bool _openslide_czi_is_multi_illumination( _openslide_czi * czi );
static bool _openslide_czi_is_multi_rotation( _openslide_czi * czi );
static bool _openslide_czi_is_multi_time( _openslide_czi * czi );
static bool _openslide_czi_is_multi_zslice( _openslide_czi * czi );
static bool _openslide_czi_is_multi_channel( _openslide_czi * czi );
static bool _openslide_czi_has_data_uncompressed( _openslide_czi * czi ) G_GNUC_UNUSED;
static bool _openslide_czi_has_data_jpg( _openslide_czi * czi ) G_GNUC_UNUSED;
static bool _openslide_czi_has_data_jpgxr( _openslide_czi * czi ) G_GNUC_UNUSED;
static bool _openslide_czi_has_data_lzw( _openslide_czi * czi );
static bool _openslide_czi_has_data_cameraspec( _openslide_czi * czi );
static bool _openslide_czi_has_data_systemspec( _openslide_czi * czi );

// Tiles
static int32_t            _openslide_czi_get_roi_count( _openslide_czi * czi ) G_GNUC_UNUSED;
static int32_t            _openslide_czi_get_level_count( _openslide_czi * czi );
static int32_t            _openslide_czi_get_level_subsampling( _openslide_czi * czi, int32_t level, GError **err );
static struct _czi_tile * _openslide_czi_get_level_tile( _openslide_czi * czi, int32_t level, int64_t uid, GError **err );
static bool               _openslide_czi_get_level_tile_size( _openslide_czi * czi, int32_t level, int32_t * w, int32_t * h, GError ** err );
static bool               _openslide_czi_get_level_tile_offset( _openslide_czi * czi, int32_t level, int32_t * x, int32_t * y, GError ** err );
static uint8_t *          _openslide_czi_get_level_tile_data( _openslide_czi * czi, int32_t level, int64_t uid, int32_t * buffer_size, GError **err );
static GList   *          _openslide_czi_get_level_tiles( _openslide_czi * czi, int32_t level, GError **err );
static bool               _openslide_czi_free_level_tile_data( _openslide_czi * czi, int32_t level, int64_t uid, GError **err );
static void               _openslide_czi_free_list_tiles( GList * list );
static uint8_t *          _openslide_czi_uncompress_tile( struct _openslide_czi_tile_descriptor * tile_desc, uint8_t * data, int32_t data_size, int32_t * uncompressed_data_size, GError ** err);
static uint8_t *          _openslide_czi_load_tile( _openslide_czi * czi, int32_t level, int64_t uid, int32_t * buffer_size, GError **err );
static uint8_t *          _openslide_czi_data_convert_to_rgba32( enum czi_pixel_t pixel_type, uint8_t * tile_data, int32_t tile_data_size, int32_t * converted_tile_data_size, GError ** err);
static uint8_t            _openslide_czi_pixel_type_size( enum czi_pixel_t );
static uint8_t            _openslide_czi_pixel_type_channel_count( enum czi_pixel_t type );
static bool               _openslide_czi_destroy_tile( _openslide_czi * czi, int32_t level, int64_t uid, GError **err ) G_GNUC_UNUSED;

// Metadata
// There is one metadata block per file. In the multi-file case, I guess
// the same metadata block is stored in each file.
// Still, we give the possibility to choose which metadata block to load.
// In all cases at least one metadata block is present, so calling the
// load method with index 0 should always return something.
static int32_t   _openslide_czi_get_metadata_count( _openslide_czi * czi );
static char    * _openslide_czi_load_metadata( _openslide_czi * czi, int32_t index, int32_t * buffer_size, GError **err );
static bool      _openslide_czi_destroy_metadata( _openslide_czi * czi, int32_t index, GError **err );
/*TODO*/ //static char    * _openslide_czi_load_subblock_metadata( _openslide_czi * czi, int32_t index, int32_t * buffer_size, GError **err ) G_GNUC_UNUSED;
/*TODO*/ //static bool      _openslide_czi_destroy_subblock_metadata( _openslide_czi * czi, int32_t index, GError **err ) G_GNUC_UNUSED;
// Attachments
// If a null pointer is returned along with no error, it means that the
// attachment is not stored in the file
/*TODO*/static _openslide_czi * _openslide_czi_decode_label( _openslide_czi * czi, GError **err ) G_GNUC_UNUSED;
/*TODO*/static _openslide_czi * _openslide_czi_decode_prescan( _openslide_czi * czi, GError **err ) G_GNUC_UNUSED;
/*TODO*/static _openslide_czi * _openslide_czi_decode_slide_preview( _openslide_czi * czi, GError **err ) G_GNUC_UNUSED;

// Openslide utils
static int32_t   _openslide_get_level_index( openslide_t * osr, struct _openslide_level * level );
static bool      _openslide_get_resolution( openslide_t * osr, double * mppx, double * mppy, GError **err ) G_GNUC_UNUSED;

// key:PARSING-PRI-DECL
//============================================================================
//
//               ZISRAW PARSING PRIVATE METHODS : DECLARATIONS
//
//============================================================================

//============================================================================
//   PRIVATE MACROS
//============================================================================

#define TRY_READ_ITEMS( buf, count, size, stream, err, prefix )              \
  if( !read_items( (void*)buf, count, size, stream, err ) ) {                \
    if( prefix ) g_prefix_error( err, prefix );                              \
    return false;                                                            \
  } else (void)0

#define TRY_FSEEKO( stream, offset, flag, err, prefix )                      \
  if( fseeko( stream, offset, flag ) ) {                                     \
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,               \
    "Failed to move in file: %s", strerror( errno ) );                       \
    if( prefix ) g_prefix_error( err, prefix );                              \
    return false;                                                            \
  } else (void)0


//============================================================================
//   PRIVATE ENUMS
//============================================================================

enum _czi_data_t {
  DATA_TYPE_UNKNOWN       = -1,
  U8_TYPE                 = 0,
  U16_TYPE                = 1,
  U32_TYPE                = 2,
  U64_TYPE                = 3,
  FLOAT_TYPE              = 4,
  CFLOAT_TYPE             = 5
};


enum _czi_accumulator_t {
  MIN_ACCUMULATOR         = 0,
  MAX_ACCUMULATOR         = 1
};

//============================================================================
//   PRIVATE STRUCTURES
//============================================================================

// In ZISRAW, each segment is preceded by a header wich specify the segment
// type and its size. It is used to navigate in the file.
struct _czi_segment_header {
  char    id[16];
  int64_t allocated_size;
  int64_t used_size;
};

// This structure describes a data source.
// A data source can be either :
// - a file, in which case the 'stream' is opened with _openslide_fopen,
//   'begin' is 0 and 'size' is int64_t::MIN
// - a stream (for a czi embedded as attachment for example), in which case
//   'filename' is NULL, 'begin' specifies the stream start position from
//   SEEK_SET, and 'size' specifies the number of usable bytes for our stream.
struct _czi_source {
  char    * filename;
  FILE    * stream;
  int64_t   begin;
  int64_t   size;
};

struct _czi {
  GPtrArray   * sources;                            // struct _czi_source
  bool          is_multi_view;                      // precomputed information
  bool          is_multi_phase;                     // ""
  bool          is_multi_block;                     // ""
  bool          is_multi_illumination;              // ""
  bool          is_multi_scenes;                    // ""
  bool          is_multi_rotation;                  // ""
  bool          is_multi_time;                      // ""
  bool          is_multi_zslice;                    // ""
  bool          is_multi_channel;                   // ""
  bool          has_data_uncompressed;              // ""
  bool          has_data_jpg;                       // ""
  bool          has_data_jpgxr;                     // ""
  bool          has_data_lzw;                       // ""
  bool          has_data_cameraspec;                // ""
  bool          has_data_systemspec;                // ""
  GPtrArray   * file_headers;                       // struct _czi_file_header
  GPtrArray   * levels;                             // struct _czi_level
  GPtrArray   * rois;                               // struct _czi_roi
  GPtrArray   * metadata;                           // struct _czi_metadata
  GHashTable  * attachments;                        // key: guid - value: struct _czi_attachment
  GHashTable  * grids;                              // key: downsample - value: openslide_grid

#ifdef CZI_DEBUG
  GHashTable  * tileread_counts;                    // key: guid - value: int64_t
  GHashTable  * tilecached_counts;                  // key: guid - value: int64_t
#endif
};

// One per actual file
struct _czi_file_header {
  struct _czi        * czi;                               // parent, not owned
  struct _czi_source * source;                                // not owned (?)
  int32_t              major;
  int32_t              minor;
  uint8_t              primary_file_guid[16];
  uint8_t              file_guid[16];
  int32_t              file_part;
  int64_t              directory_position;      // offset to directory segment
  int64_t              metadata_position;        // offset to metadata segment
  bool                 update_pending;
  int64_t              attdir_position;      // offset to attachment directory
};

// Structure that stores ROI information for a level
struct _czi_roi {
  struct _czi                 * czi; // parent, not owned
  enum czi_roi_shape_t          shape; // ROI shape
  GHashTable                  * shapeparams; // Parameters to process ROI shape
  enum czi_roi_covering_mode_t  covering_mode; // Covering mode
  double                        overlap; // Overlap between tiles
  double                        x; // X top left position
  double                        y; // Y top left position
  double                        w; // Width
  double                        h; // Height
  int32_t                       rows; // number of tile rows
  int32_t                       columns; // number of tile columns
};

// Not actually stored this way in the format, but it makes it is easier
// to manipulate pyramid levels.
struct _czi_level {
  struct _czi           * czi;                            // parent, not owned
  enum czi_pixel_t        pixel_type;
  enum czi_compression_t  compression;
  enum czi_pyramid_t      pyramid_type;
  int32_t                 subsampling_x;
  int32_t                 subsampling_y;
  GHashTable            * size;   // key: char * XYCZTRSIBMHV - value: int32_t
  GHashTable            * start;  // key: char * XYCZTRSIBMHV - value: int32_t
  GHashTable            * tiles;  // key: guid - value: struct _czi_tile
};

// Each tile is stored in a subblock segment, even though we only used headers
// stored in the directory segment at first to compute information. Actual
// data is only loaded when asked for.
struct _czi_tile {                    // subblock_segment + directory_entry_dv
  struct _czi_level     * level;                          // parent, not owned
  struct _czi_source    * source;                             // not owned (?)
  int32_t                 file_part;     // could be used in multi file case ?
  int64_t                 tile_offset;
  int64_t                 uid;
  enum czi_pixel_t        pixel_type;
  enum czi_compression_t  compression;
  enum czi_pyramid_t      pyramid_type;
  GHashTable            * dimensions;   // key: char * - struct _czi_dimension
  int32_t                 directory_size;
  int32_t                 metadata_size;
  int32_t                 data_size;
  int32_t                 attachment_size;
  char                  * metadata_buf;              // only loaded when asked
  uint8_t               * data_buf;                  // only loaded when asked
  uint8_t               * attachment_buf;            // only loaded when asked
};

struct _czi_dimension {                                  // dimension_entry_dv
  struct _czi_tile * tile;
  char               dimension_id[5];
  int32_t            start;
  int32_t            size;
  float              start_coordinate;
  int32_t            stored_size;
};

struct _czi_metadata {
  struct _czi         * czi;
  struct _czi_source  * source;
  int64_t               offset;
  int32_t               xml_size;
  int32_t               attachment_size;
  char                * xml_buf;
  uint8_t             * attachment_buf;
};

struct _czi_attachment {
  struct _czi         * czi;
  struct _czi_source  * source;
  int64_t               file_position;
  int32_t               file_part;
  uint8_t               content_guid[16];
  char                  content_file_type[8];
  char                  name[80];
  int32_t               data_size;
  uint8_t             * data;
};

struct _czi_pixel_dynamic_info;
/*
typedef void (*_czi_pixel_dynamic_info_update_min_max_t)(
                    struct _czi_pixel_dynamic_info    * di,
                    uint8_t                           * buffer);
*/

// Structure used to accumulate data
struct _czi_accumulator {
  //--- methods ----------------------------------------------------------------
  void (*accumulate)(struct _czi_accumulator              * ca,
                     uint64_t                               pos,
                     uint8_t                              * buffer);

  //--- attributes -------------------------------------------------------------  
  enum _czi_data_t                                          data_type;
  uint8_t                                                   data_type_size;
  uint64_t                                                  data_count;
  uint8_t                                                 * data;
};

// Structure used to declare static accumulator functions
struct _czi_accumulator_func {
  void (*accumulate)(struct _czi_accumulator              * ca,
                     uint64_t                               pos,
                     uint8_t                              * buffer);
};

struct _czi_rescale_info {
  //--- methods ----------------------------------------------------------------

  //--- attributes -------------------------------------------------------------
  double                                                    shift;
  double                                                    slope;
};

struct _czi_rescale_info_func {
  //--- methods ----------------------------------------------------------------
  struct _czi_rescale_info * (*rescale_info)(
                     struct _czi_pixel_dynamic_info       * cpdi,
                     GError                              ** err);
  
  //--- attributes -------------------------------------------------------------
};

// Structure used to update dynamic information (minimum and maximum)
// from a buffer.
struct _czi_pixel_dynamic_info {
  //--- methods ----------------------------------------------------------------
  void (*update)(
                     struct _czi_pixel_dynamic_info       * cpdi,
                     uint8_t                              * buffer,
                     uint64_t                               buffer_size,
                     GError                              ** err);
/*  
  struct _czi_rescale_info * (*rescale_info)(
                     struct _czi_pixel_dynamic_info       * cpdi,
                     GError                              ** err);
*/
  //--- attributes -------------------------------------------------------------
  enum czi_pixel_t                                          type;
  uint8_t                                                 * min_per_channel; 
  uint8_t                                                 * max_per_channel;
  
  // For acceleration purpose
  uint8_t                                                   channel_count;
  uint8_t                                                   channel_size;
}; 

// Structure used to convert a buffer from a channel type to another.
// It uses dynamic information to rescale dynamic of the data if needed (i.e.
// when destination type is less precise than source type).
struct _czi_channel_converter {
  //--- methods ----------------------------------------------------------------
  void (*convert)(const struct _czi_channel_converter     * ccc,
                  const struct _czi_rescale_info          * cri,
                  uint8_t                                 * src_buffer,
                  uint8_t                                 * dest_buffer,
                  GError                                 ** err);
  
  //--- attributes -------------------------------------------------------------
  enum _czi_data_t   src_channel_type;
  enum _czi_data_t   dest_channel_type;
};

// Structure used to convert a buffer from a pixel type to another.
// It uses dynamic information to rescale dynamic of the data if needed (i.e.
// when destination type is less precise than source type).
struct _czi_pixel_converter {
  //--- methods ----------------------------------------------------------------
  void (*convert)(const struct _czi_pixel_converter       * cpc,
                  const struct _czi_rescale_info          * cri,
                  uint8_t                                 * src_buffer,
                  uint8_t                                 * dest_buffer,
                  GError                                 ** err);
  
  //--- attributes -------------------------------------------------------------
  enum czi_pixel_t      src_pixel_type;
  enum czi_pixel_t      dest_pixel_type;
};


//============================================================================
//   PRIVATE STRINGS
//============================================================================

// segments
const int32_t CZI_ALIGNMENT     = 32;
const int32_t CZI_HEADER_SIZE   = 32;
const char *  CZI_FILE          = "ZISRAWFILE";
const char *  CZI_DIRECTORY     = "ZISRAWDIRECTORY";
const char *  CZI_SUBBLOCK      = "ZISRAWSUBBLOCK";
const char *  CZI_METADATA      = "ZISRAWMETADATA";
const char *  CZI_ATTACH        = "ZISRAWATTACH";
const char *  CZI_ATTDIR        = "ZISRAWATTDIR";
const char *  CZI_DELETED       = "DELETED";

// czi content types
const char * czi_content_zip    = "ZIP";     // ZIP stream
const char * czi_content_zisraw = "ZISRAW";  // ZISRAW file
const char * czi_content_czi    = "CZI";     // CZI file
const char * czi_content_czexp  = "CZEXP";   // XML experiment definition
const char * czi_content_czhws  = "CZHWS";   // XML hardware settings
const char * czi_content_czmvm  = "CZMVM";   // XML multiview microscopy info
const char * czi_content_cztims = "CZTIMS";  // BIN Time stamp list
const char * czi_content_czeval = "CZEVL";   // BIN Event list
const char * czi_content_czlut  = "CZLUT";   // BIN Lookup table
const char * czi_content_czpml  = "CZPML";   // BIN Pal molecule list
const char * czi_content_czfoc  = "CZFOC";   // BIN Focus positions
const char * czi_content_jpg    = "JPG";     // JPEG stream
// Plus: spec says any MIME type is possible, but given example do not
// follow mime format: JPG instead of image/jpeg, ...

// czi reserved attachment names
const char * czi_att_thumb   = "Thumbnail";       // JPG
const char * czi_att_pvw     = "Preview";         // Any media type (JPG)
const char * czi_att_exp     = "Experiment";      // CZEXP
const char * czi_att_hws     = "HardwareSetting"; // CZHWS
const char * czi_att_ts      = "TimeStamps";      // CZTIMS
const char * czi_att_event   = "EventList";       // CZEVL
const char * czi_att_lut     = "LookupTables";    // CZLUT
const char * czi_att_pml     = "PalMoleculeList"; // CZPML
const char * czi_att_focus   = "FocusPositions";  // CZFOC
const char * czi_att_mvm     = "MVM";             // CZMVM
const char * czi_att_label   = "Label";           // CZI
const char * czi_att_prescan = "Prescan";         // CZI
const char * czi_att_slpvw   = "SlidePreview";    // CZI

// czi reserved covering modes
const char * czi_cov_aligned_to_global_grid       = "AlignedToGlobalGrid";       // Global grid alignement
const char * czi_cov_aligned_to_local_tile_region = "AlignedToLocalTileRegion";  // Local tile region alignement

// strings for enums
// czi_pixel_t
const char * pxl_unknown                   = "PXL_UNKNOWN";
const char * gray_8                        = "GRAY_8";
const char * gray_16                       = "GRAY_16";
const char * gray_32_float                 = "GRAY_32_FLOAT";
const char * bgr_24                        = "BGR_24";
const char * bgr_48                        = "BGR_48";
const char * bgr_96_float                  = "BGR_96_FLOAT";
const char * bgra_32                       = "BGRA_32";
const char * gray_64_complex_float         = "GRAY_64_COMPLEX_FLOAT";
const char * bgr_192_complex_float         = "BGR_192_COMPLEX_FLOAT";
const char * gray_32                       = "GRAY_32";
const char * gray_64                       = "GRAY_64";

// czi_compression_t
const char * cmp_unknown                   = "CMP_UNKNOWN";
const char * uncompressed                  = "UNCOMPRESSED";
const char * jpeg                          = "JPEG";
const char * lzw                           = "LZW";
const char * jpegxr                        = "JPEGXR";
const char * camera_spec                   = "CAMERA_SPEC";
const char * system_spec                   = "SYSTEM_SPEC";

// czi_pyramid_t
const char * pyr_unknown                   = "PYR_UNKNOWN";
const char * none                          = "NONE";
const char * single                        = "SINGLE";
const char * multi                         = "MULTI";

// czi_roi_shape_t
const char * shp_unknown                   = "SHP_UNKNOWN";
const char * ellipse                       = "ELLIPSE";
const char * rectangle                     = "RECTANGLE";
const char * polygon                       = "POLYGON";

// czi_roi_covering_mode_t
const char * cov_unknown                   = "SHP_UNKNOWN";
const char * aligned_to_global_grid        = "ALIGNED_TO_GLOBAL_GRID";
const char * aligned_to_local_tile_region  = "ALIGNED_TO_LOCAL_TILE_REGION";

//============================================================================
//   PRIVATE METHODS
//============================================================================

//--- generic utils ----------------------------------------------------------
// Apply byte swapping on an array of items.
static bool do_byte_swap(
  uint8_t   * items,    // Pointer to memory block to process
  uint64_t    count,    // Number of items in the array.
  size_t      size      // Size of one item
);

// Reads a series of items from file and eventually applies byte swapping.
// Byte swapping is applied when G_BYTE_ORDER == G_BIG_ENDIAN is true,
// since data in a CZI file are stored in little endian.
static bool read_items(
  void     * items,    // Pointer to memory block to process
  uint64_t   count,    // Number of items in the array
  size_t     size,     // Size of one item
  FILE     * stream,   // File stream,
  GError  ** err       // Error handling
);

// Compare two guid to check that they are equal or not.
// Return true if all bytes are equals and false otherwise.
static bool compare_guid(
    uint8_t * guid1, // First GUID to compare
    uint8_t * guid2  // Second GUID to compare
);

//--- read _czi --------------------------------------------------------------
static bool czi_find_sources( const char * filename, struct _czi * czi, GError ** err );
static bool czi_decode_one_stream( struct _czi_source * source, struct _czi * czi, GError ** err );
static bool czi_add_file_header( struct _czi * czi, struct _czi_file_header * header, GError ** err );
static bool czi_add_tile( struct _czi * czi, struct _czi_tile * tile, int32_t ss_x, int32_t ss_y, GError ** err );
static bool czi_update_bool_dimension( struct _czi * czi, char key, int32_t size, GError ** err );
static bool czi_update_bool_compression( struct _czi * czi, enum czi_compression_t compression, GError ** err );
static int32_t czi_cmp_level( const struct _czi_level ** l1, const struct _czi_level ** l2 );

//--- new --------------------------------------------------------------------
static struct _czi                              * czi_new( GError ** err );
static struct _czi_source                       * czi_new_source( GError ** err );
static struct _czi_file_header                  * czi_new_file_header( struct _czi * czi, GError ** err );
static struct _czi_level                        * czi_new_level( struct _czi * czi, GError ** err );
static struct _czi_roi                          * czi_new_roi( struct _czi * czi, GError ** err );
static struct _czi_metadata                     * czi_new_metadata( struct _czi * czi, GError ** err );
/*TODO*/static struct _czi_attachment           * czi_new_attachment( struct _czi * czi, GError ** err ) G_GNUC_UNUSED;
static struct _czi_tile                         * czi_new_tile( GError ** err );
static struct _czi_dimension                    * czi_new_dimension( struct _czi_tile * tile, GError ** err );
static int16_t                                  * czi_new_S16( int16_t integer, GError ** err ) G_GNUC_UNUSED;
static int32_t                                  * czi_new_S32( int32_t integer, GError ** err );
static int64_t                                  * czi_new_S64( int64_t integer, GError ** err );
static struct _openslide_czi_tile_descriptor    * czi_new_tile_descriptor( struct _czi_tile * tile, GError ** err );
static struct _czi_pixel_dynamic_info           * czi_new_pixel_dynamic_info(enum czi_pixel_t pixel_type, GError ** err );
static struct _czi_accumulator                  * czi_new_accumulator(enum _czi_accumulator_t accumulator_type, enum _czi_data_t data_type, uint64_t data_count, GError ** err );
static struct _czi_rescale_info                 * czi_new_rescale_info(GError ** err G_GNUC_UNUSED);

//--- free -------------------------------------------------------------------
static void czi_free(                      struct _czi                           * ptr );
static void czi_free_source(               struct _czi_source                    * ptr );
static void czi_free_file_header(          struct _czi_file_header               * ptr );
static void czi_free_level(                struct _czi_level                     * ptr );
static void czi_free_metadata(             struct _czi_metadata                  * ptr );
static void czi_free_attachment(           struct _czi_attachment                * ptr ) G_GNUC_UNUSED;
static void czi_free_tile(                 struct _czi_tile                      * ptr );
static void czi_free_dimension(            struct _czi_dimension                 * ptr );
static void czi_free_roi(                  struct _czi_roi                       * ptr );
static void czi_free_S16(                  int16_t                               * ptr ) G_GNUC_UNUSED;
static void czi_free_S32(                  int32_t                               * ptr );
static void czi_free_S64(                  int64_t                               * ptr );
static void czi_free_tile_descriptor(      struct _openslide_czi_tile_descriptor * ptr );
static void czi_free_pixel_dynamic_info(   struct _czi_pixel_dynamic_info        * ptr );
static void czi_free_accumulator(          struct _czi_accumulator               * ptr );
static void czi_free_rescale_info(         struct _czi_rescale_info      * ptr );

//--- read -------------------------------------------------------------------
static bool czi_parse_directory(  struct _czi_source * source, struct _czi * czi,                     GError ** err );
/*TODO*/static bool czi_parse_attdir(     struct _czi_source * source, struct _czi * czi,                     GError ** err ) G_GNUC_UNUSED;
static bool czi_read_file_header( struct _czi_source * source, struct _czi_file_header * file_header, GError ** err );
static bool czi_read_metadata(    struct _czi_source * source, struct _czi_metadata * metadata,       GError ** err );
static bool czi_read_tile(        struct _czi_source * source, struct _czi_tile * tile,               GError ** err );
static bool czi_read_dimension(   struct _czi_source * source, struct _czi_dimension * dimension,     GError ** err );

//--- enum string conversion ------------------------------------------------------
static const char * czi_compression_t_string(       enum czi_compression_t compression_type );
static const char * czi_pixel_t_string(             enum czi_pixel_t pixel_type );
static const char * czi_pyramid_t_string(           enum czi_pyramid_t pyramid_type );
static const char * czi_roi_shape_t_string(         enum czi_roi_shape_t roi_shape_type );
static const char * czi_roi_covering_mode_t_string( enum czi_roi_covering_mode_t roi_covering_mode );
static const char * czi_boolean_t_string(           bool b );

//--- display ----------------------------------------------------------------
/*TODO*/static void czi_display(                      struct _czi                           * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_source(               struct _czi_source                    * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_segment_header(       struct _czi_segment_header            * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_file_header(          struct _czi_file_header               * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_roi(                  struct _czi_roi                       * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_level(                struct _czi_level                     * ptr, uint16_t alignment ) G_GNUC_UNUSED;
/*TODO*/static void czi_display_metadata(             struct _czi_metadata                  * ptr, uint16_t alignment ) G_GNUC_UNUSED;
/*TODO*/static void czi_display_attachment(           struct _czi_attachment                * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_tile(                 struct _czi_tile                      * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_dimension(            struct _czi_dimension                 * ptr, uint16_t alignment ) G_GNUC_UNUSED;
static void czi_display_tile_descriptor(      struct _openslide_czi_tile_descriptor * ptr, uint16_t alignment ) G_GNUC_UNUSED;

//--- navigate in structure --------------------------------------------------
static bool czi_read_next_segment_header(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  GError                     ** err
);
static bool czi_read_next_segment_header_with_id(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  const char                  * id,
  GError                     ** err
) G_GNUC_UNUSED;
static bool czi_skip_segment(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  GError                     ** err
);
static bool czi_is_zisraw(
  FILE                        * stream,
  GError                     ** err
);

//--- accumulator function -----------------------------------------------------
static int32_t czi_accumulator_func_uid(
                        enum _czi_accumulator_t                type,
                        enum _czi_data_t                       data_type);

static GHashTable * czi_accumulator_func_hash_table(
                        GError                              ** err);

static const struct _czi_accumulator_func * czi_get_accumulator_func(
                        enum _czi_accumulator_t                type,
                        enum _czi_data_t                       data_type);

//--- accumulator --------------------------------------------------------------
static void czi_buffer_accumulate(
                        enum _czi_accumulator_t                accumulator_type,
                        uint8_t                              * buffer,
                        uint8_t                              * result,
                        enum _czi_data_t                       data_type,
                        uint64_t                               data_count,
                        GError                              ** err);

//--- minimum ------------------------------------------------------------------
static void czi_accumulator_min_accumulate_U8(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_min_accumulate_U16(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_min_accumulate_U32(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_min_accumulate_U64(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_min_accumulate_FLOAT(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_min_accumulate_CFLOAT(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

//--- maximum ------------------------------------------------------------------
static void czi_accumulator_max_accumulate_U8(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_max_accumulate_U16(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_max_accumulate_U32(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_max_accumulate_U64(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_max_accumulate_FLOAT(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

static void czi_accumulator_max_accumulate_CFLOAT(
                        struct _czi_accumulator              * ac,
                        uint64_t                               pos,
                        uint8_t                              * buffer
);

//--- pixel dynamic info -------------------------------------------------------
//static int32_t czi_pixel_dynamic_info_updater_uid(
//                        enum czi_pixel_t                       type);

static void czi_pixel_dynamic_info_update(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer,
                        uint64_t                               buffer_size,
                        GError                              ** err);

/*
static void czi_pixel_dynamic_info_update_min_max_multichannel(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);*/

//static GHashTable * czi_pixel_dynamic_info_updater_hash_table(
//                        GError                              ** err);

//--- pixel dynamic info update min max ----------------------------------------
/*
void czi_pixel_dynamic_info_update_min_max_U8(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);

void czi_pixel_dynamic_info_update_min_max_U16(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);

void czi_pixel_dynamic_info_update_min_max_U32(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);

void czi_pixel_dynamic_info_update_min_max_U64(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);

void czi_pixel_dynamic_info_update_min_max_FLOAT(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);

void czi_pixel_dynamic_info_update_min_max_CFLOAT(
                        struct _czi_pixel_dynamic_info       * pdi,
                        uint8_t                              * buffer
);
*/

//--- converter ----------------------------------------------------------------
static int32_t czi_uid_S32(
                        uint8_t                                b0,
                        uint8_t                                b1,
                        uint8_t                                b2,
                        uint8_t                                b3
);

//--- buffer convert -----------------------------------------------------------
static void czi_buffer_convert_U8_to_U8(
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        double                                 shift,
                        double                                 scale
);

static void czi_buffer_convert_U16_to_U8(
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        double                                 shift,
                        double                                 scale
);

static void czi_buffer_convert_U32_to_U8(
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        double                                 shift,
                        double                                 scale
);

static void czi_buffer_convert_U64_to_U8(
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        double                                 shift,
                        double                                 scale
);

static void czi_buffer_convert_FLOAT_to_U8(
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        double                                 shift,
                        double                                 scale
);

static void czi_buffer_convert_CFLOAT_to_U8(
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        double                                 shift,
                        double                                 scale
);

//--- typed convert ------------------------------------------------------------
static uint8_t czi_convert_U8_to_U8(
                        uint8_t                                value,
                        double                                 shift,
                        double                                 scale
) G_GNUC_UNUSED;

static uint8_t czi_convert_U16_to_U8(
                        uint16_t                               value,
                        double                                 shift,
                        double                                 scale
) G_GNUC_UNUSED;

static uint8_t czi_convert_U32_to_U8(
                        uint32_t                               value,
                        double                                 shift,
                        double                                 scale
) G_GNUC_UNUSED;

static uint8_t czi_convert_U64_to_U8(
                        uint64_t                               value,
                        double                                 shift,
                        double                                 scale
) G_GNUC_UNUSED;

static uint8_t czi_convert_FLOAT_to_U8(
                        float                                  value,
                        double                                 shift,
                        double                                 scale
) G_GNUC_UNUSED;

static uint8_t czi_convert_CFLOAT_to_U8(
                        complex float                          value,
                        double                                 shift,
                        double                                 scale
) G_GNUC_UNUSED;

//--- data type ----------------------------------------------------------------
static uint8_t czi_data_type_size(
                        enum _czi_data_t                       type);

static enum _czi_data_t czi_data_type(
                        enum czi_pixel_t                       pixel_type);

//--- channel converter --------------------------------------------------------
static int32_t czi_channel_converter_uid(
                        enum _czi_data_t                       src_type,
                        enum _czi_data_t                       dest_type);

static const struct _czi_channel_converter * czi_get_channel_converter(
                        enum _czi_data_t                       src_type,
                        enum _czi_data_t                       dest_type)
G_GNUC_UNUSED;

static GHashTable * czi_channel_converter_hash_table(
                        GError                              ** err);

//--- channel conversion -------------------------------------------------------
static void czi_channel_convert_U8_to_U8(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_channel_convert_U16_to_U8(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_channel_convert_U32_to_U8(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_channel_convert_U64_to_U8(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_channel_convert_FLOAT_to_U8(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_channel_convert_CFLOAT_to_U8(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);
//--- rescale info func --------------------------------------------------------
static GHashTable * czi_rescale_info_func_hash_table(GError ** err);

static uint32_t czi_rescale_info_func_uid(
                        enum _czi_data_t                      src_type,
                        enum _czi_data_t                      dest_type);

static const struct _czi_rescale_info_func * czi_get_rescale_info_func(
                        enum _czi_data_t                      src_type,
                        enum _czi_data_t                      dest_type);

//--- rescale info -------------------------------------------------------------
static struct _czi_rescale_info * czi_rescale_info_U16_to_U8(
                        struct _czi_pixel_dynamic_info      * cpdi,
                        GError                             ** err);

static struct _czi_rescale_info * czi_rescale_info_FLOAT_to_U8(
                        struct _czi_pixel_dynamic_info      * cpdi,
                        GError                             ** err);

//--- pixel --------------------------------------------------------------------

//--- pixel converter ----------------------------------------------------------
static GHashTable * czi_pixel_converter_hash_table(GError  ** err);

static uint32_t czi_pixel_converter_uid(
                        enum czi_pixel_t                      src_type,
                        enum czi_pixel_t                      dest_type);
    
static const struct _czi_pixel_converter * czi_get_pixel_converter(
                        enum czi_pixel_t                      src_type,
                        enum czi_pixel_t                      dest_type);

//--- pixel conversion ---------------------------------------------------------
static void czi_pixel_convert_multichannel(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        uint8_t                                channel_count,
                        GError                              ** err);

static void czi_pixel_convert_BGR_24_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_pixel_convert_BGRA_32_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_pixel_convert_BGR_48_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_pixel_convert_BGR_96_FLOAT_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

static void czi_pixel_convert_BGR_192_COMPLEX_FLOAT_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err);

// key:PARSING-PUB-DEF
//============================================================================
//
//                        PUBLIC API DEFINITIONS
//
//============================================================================

//============================================================================
//    GENERIC UTILS (move them after ?)
//============================================================================

int32_t _openslide_get_level_index( openslide_t * osr,
                                    struct _openslide_level * level )
{
  struct _openslide_level * levelcur;

  // Search level index in levels based upon the downsample
  for (int32_t l = 0; l < osr->level_count; ++l) {
    levelcur = osr->levels[l];
    if (levelcur->downsample == level->downsample) {
      // Matching level found
      return l;
    }
  }

  return -1;
}

bool do_byte_swap(
  uint8_t   * items,
  uint64_t    count,
  size_t      size
)
{
  uint64_t k;
  size_t   b;
  uint8_t  tmp;
  for( k = 0; k < count*size; k += size )
    for( b=0; b < size/2; ++b )
    {
      tmp = items[k+b];
      items[k+b] = items[k+size-1-b];
      items[k+size-1-b] = tmp;
    }
  return true;
}

bool read_items(
  void     * items,
  uint64_t   count,
  size_t     size,
  FILE     * stream,
  GError  ** err
)
{
  g_assert( stream );
  uint64_t len;
  len = fread( items, size, count, stream );
  if( len != count ) {
    char * out;
    if( feof(stream) )
      out = "reached end of file";
    else if( ferror(stream) )
      out = strerror( errno );
    else
      out = "unknown error";
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Could only read %li out of %li items: %s",
                 len, count, out );
    return false;
  }
  if( G_BYTE_ORDER == G_BIG_ENDIAN ) do_byte_swap( items, len, size );
  return true;
}

bool compare_guid(uint8_t * guid1, uint8_t * guid2) {
    for( int i=0; i<16; ++i ) {
        if (guid1[i] != guid2[i])
            return false;
    }
    
    return true;
}

bool _openslide_get_resolution( openslide_t * osr, double * mppx, double * mppy, GError **err ) {
  *mppx = _openslide_parse_double( (const char*)g_hash_table_lookup( osr->properties, OPENSLIDE_PROPERTY_NAME_MPP_X ));
  if (!mppx) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find x resolution" );
    return false;
  }

  *mppy = _openslide_parse_double( (const char*)g_hash_table_lookup( osr->properties, OPENSLIDE_PROPERTY_NAME_MPP_Y ));
  if (!mppy) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find y resolution" );
    return false;
  }

  return true;
}

//============================================================================
//   READ CZI
//============================================================================

bool czi_find_sources(
  const char   * filename,
  struct _czi  * czi,
  GError      ** err
)
{
  //g_debug( "czi_find_sources" );
  g_assert( filename );
  g_assert( czi );
  struct _czi_source * source;

  source = czi_new_source( err );
  if( !source ) return false;
  g_ptr_array_add( czi->sources, source );
  source->filename = (char*) g_strdup( filename );
  source->begin = 0;
  source->size = 0;
  source->stream = _openslide_fopen( filename, "rb", err );
  if( !source->stream ) return false;

  // Look for eventual part files
  int32_t i = 1;
  char * base = g_strndup( filename, strlen(filename) - strlen(".czi") );
  char * partname = g_strdup_printf( "%s (%d).czi", base, i );
  while( g_file_test( partname, G_FILE_TEST_EXISTS ) )
  {
    g_debug( "Found part file %s", partname );
    source = czi_new_source( err );
    if( !source ) return false;
    g_ptr_array_add( czi->sources, source );
    source->filename = partname;
    source->begin = 0;
    source->size = 0;
    source->stream = _openslide_fopen( partname, "rb", err );
    if( !source->stream ) {
      g_free( base );
      return false;
    }
  }
  g_free( partname );
  g_free( base );

  return true;
}

bool czi_decode_one_stream(
  struct _czi_source  * source,
  struct _czi         * czi,
  GError             ** err
)
{
  ///g_debug( "czi_decode_one_stream" );
  g_assert( source );
  g_assert( czi );

  // open stream
  if( !source->stream ) {
    if( !source->filename ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Need a stream or a file" );
      return false;
    } else {
      source->stream = _openslide_fopen( source->filename, "rb", err );
      if( !source->stream ) return false;
    }
  }

  // go to stream beginning
  if( fseeko( source->stream, source->begin, SEEK_SET ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to seek position %ld: %s",
                 source->begin, g_strerror(errno) );
    return false;
  }

  if( !czi_is_zisraw( source->stream, err ) )
    return false;

  // allocate segment header
  struct _czi_segment_header * header = (struct _czi_segment_header*) 
                                        g_slice_alloc0( 
                                            sizeof(struct _czi_segment_header)
                                        );
  // read segment header
  if (!czi_read_next_segment_header(source, header, err)) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to read segment header at the position %ld: %s",
                 source->begin, g_strerror(errno) );
    g_slice_free( struct _czi_segment_header, header );
    return false;
  }
  
  if (!strcmp( header->id, CZI_FILE )) {
      
    // read file header
    struct _czi_file_header * file_header = czi_new_file_header(czi, err);
    
    if (!czi_read_file_header(source, file_header, err)) {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Failed to read file header at the position %ld: %s",
                    source->begin, g_strerror(errno) );
        g_slice_free( struct _czi_segment_header, header );
        czi_free_file_header(file_header);
        return false;
    }
    
    if (!czi_add_file_header(czi, file_header, err)) {
        g_debug("File_header was not added");
        czi_free_file_header(file_header);
    }
    
    // go to metadata
    if(fseeko( source->stream, file_header->metadata_position, SEEK_SET)) {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Failed to seek metadata position %ld: %s",
                    file_header->metadata_position, g_strerror(errno) );
        g_slice_free( struct _czi_segment_header, header );
        czi_free_file_header(file_header);
        return false;
    }
    
    czi_read_next_segment_header(source, header, err);
    
    if( !strcmp( header->id, CZI_METADATA ) )
    {
      struct _czi_metadata * new_metadata = czi_new_metadata( czi, err );
      if( !new_metadata )
        return false;
      g_ptr_array_add( czi->metadata, new_metadata );
      if( !czi_read_metadata( source, new_metadata, err ) )
        return false;
    }
    
    // go to the subblock directory
    if(fseeko( source->stream, file_header->directory_position, SEEK_SET)) {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Failed to seek directory position %ld: %s",
                    file_header->directory_position, g_strerror(errno) );
        g_slice_free( struct _czi_segment_header, header );
        czi_free_file_header(file_header);
        return false;
    }
    
    bool read_next_segment = true;
    while (read_next_segment) {
        read_next_segment = czi_read_next_segment_header(source, header, err);
        
        if (!strcmp(header->id, CZI_DIRECTORY)) {
            if(!czi_parse_directory( source, czi, err )) {
                g_slice_free( struct _czi_segment_header, header );
                return false;
            }
        }
        else {
            break;
        }
    }
    
    // we assume error occuredbecause there are no segments left
    g_slice_free( struct _czi_segment_header, header );
    g_clear_error( err );
    //err = NULL;

  }
  else {
    // first segment in file must be the file header so we exit with error
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to read file header at %ld: %s",
                 source->begin, g_strerror(errno) );
    g_slice_free( struct _czi_segment_header, header );
    return false;
  }

  return true;
}

bool czi_add_file_header(
    struct _czi                   * czi,
    struct _czi_file_header       * header,
    GError                       ** err         G_GNUC_UNUSED
)
{
    //g_debug( "czi_add_file_header" );
    g_assert( czi );
    g_assert( header );
    
    struct _czi_file_header* file_header;
    
    // Check that file header was not registered yet
    for( uint32_t i=0; i < czi->file_headers->len; ++i )
    {
        file_header = (struct _czi_file_header*) g_ptr_array_index(
                                                    czi->file_headers,
                                                    i
                                                 );
        if (!compare_guid(file_header->file_guid, header->file_guid)) {
            g_ptr_array_add(czi->file_headers, header);
            return true;
        }
    }
    
    return false;
}

bool czi_add_tile(
  struct _czi       * czi,
  struct _czi_tile  * tile,
  int32_t             ss_x,
  int32_t             ss_y,
  GError           ** err
)
{
  // g_debug( "czi_add_tile" );
  g_assert( czi );
  g_assert( tile );
  struct _czi_level * level = NULL;
  struct _czi_dimension * dimension;
  gpointer has_key;
  GList * list_keys, * current_key;
  int32_t start, size;
  int32_t * cur_start, * cur_size;
  uint32_t i;

  for( i=0; i < czi->levels->len; ++i )
  {
    level = (struct _czi_level*) g_ptr_array_index( czi->levels, i );
    if( level->subsampling_x == ss_x && level->subsampling_y == ss_y )
      break;
    else
      level = NULL;
  }

  if( !level ) {
    level = czi_new_level( czi, err );
    if( !level ) return false;
    level->pixel_type    = tile->pixel_type;
    level->compression   = tile->compression;
    level->pyramid_type  = tile->pyramid_type;
    level->subsampling_x = ss_x;
    level->subsampling_y = ss_y;
    g_ptr_array_add( czi->levels, level );
  }

  g_hash_table_insert( level->tiles, czi_new_S64(tile->uid,0), tile );
  list_keys = g_hash_table_get_keys( tile->dimensions );
  current_key = list_keys;
  while( current_key )
  {
    dimension = ((struct _czi_dimension*)g_hash_table_lookup( tile->dimensions, (char*) current_key->data ));
    start = dimension->start;
    size  = dimension->size;

    has_key = g_hash_table_lookup( level->size, (char*) current_key->data );
    if( !has_key ) {
      cur_size = czi_new_S32( size, err );
      if( !cur_size ) return false;
      g_hash_table_insert( level->size, g_strdup( (char*)current_key->data ), cur_size );
      cur_start = czi_new_S32( start, err );
      if( !cur_start ) return false;
      g_hash_table_insert( level->start, g_strdup( (char*)current_key->data ), cur_start);
    } else {
      cur_start = (int32_t*) g_hash_table_lookup( level->start, (char*) current_key->data );
      cur_size  = (int32_t*) g_hash_table_lookup( level->size, (char*) current_key->data );
      // Update start and size for the level dimension
      if( start < *cur_start ) *cur_start = start;
      if( ( start + size - *cur_start ) > *cur_size ) *cur_size = (start + size - *cur_start);
    }
    if( !czi_update_bool_dimension( czi, ((char*)current_key->data)[0], *cur_size, err ) )
      return false;
    current_key = g_list_next( current_key );
  }
  if( !czi_update_bool_compression( czi, tile->compression, err ) )
    return false;

  g_list_free( list_keys );
  return true;
}

bool czi_update_bool_dimension(
  struct _czi  * czi,
  char           key,
  int32_t        size,
  GError      ** err
)
{

  if( size > 1 )
  {
    switch( key )
    {
      case 'V':
        czi->is_multi_view = true;
        break;
      case 'H':
        czi->is_multi_phase = true;
        break;
      case 'M':
        // nothing to do
        break;
      case 'B':
        czi->is_multi_block = true;
        break;
      case 'I':
        czi->is_multi_illumination = true;
        break;
      case 'S':
        czi->is_multi_scenes = true;
        break;
      case 'R':
        czi->is_multi_rotation = true;
        break;
      case 'T':
        czi->is_multi_time = true;
        break;
      case 'Z':
        czi->is_multi_zslice = true;
        break;
      case 'C':
        czi->is_multi_channel = true;
        break;
      case 'Y':
        // nothing to do
        break;
      case 'X':
        // nothing to do
        break;
      default:
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Unknown dimension %c", key );
        return false;
    }
  }

  return true;
}

bool czi_update_bool_compression(
  struct _czi             * czi,
  enum czi_compression_t    compression,
  GError                 ** err
)
{
  switch( compression )
  {
    case CMP_UNKNOWN:
      // what to do ?
      break;
    case UNCOMPRESSED:
      czi->has_data_uncompressed = true;
      break;
    case JPEG:
      czi->has_data_jpg = true;
      break;
    case LZW:
      czi->has_data_lzw = true;
      break;
    case JPEGXR:
      czi->has_data_jpgxr = true;
      break;
    case CAMERA_SPEC:
      czi->has_data_cameraspec = true;
      break;
    case SYSTEM_SPEC:
      czi->has_data_systemspec = true;
      break;
    default:
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Unknown compression %d", (int)compression );
      return false;
  }
  return true;
}

int32_t czi_cmp_level(
  const struct _czi_level ** l1,
  const struct _czi_level ** l2
)
{
  int32_t ss_x_1 = (*l1)->subsampling_x;
  int32_t ss_y_1 = (*l1)->subsampling_y;
  int32_t ss_x_2 = (*l2)->subsampling_x;
  int32_t ss_y_2 = (*l2)->subsampling_y;

  // Isn't it possible that ss_x_1 < ss_x_2
  // and ss_y_1 > ss_y_2 ?
  if( ss_x_1 < ss_x_2 )
    return -1;
  else if( ss_x_1 == ss_x_2 )
  {
    if( ss_y_1 < ss_y_2 )
      return -1;
    else if( ss_y_1 == ss_y_2 )
      return 0;
    else
      return 1;
  }
  else
    return 1;
}

//============================================================================
//   READ UTILS
//============================================================================

bool czi_read_next_segment_header(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  GError                     ** err
)
{
  g_assert( source );
  g_assert( source->stream );
  g_assert( segmentheader );

  FILE * stream = source->stream;
  off_t current_pos=-1;
  off_t previous_pos=-1;

  char id[16];
  while( !feof( stream ) )
  {
    // 32 bytes alignment
    if( ( current_pos = ftello( stream ) ) == -1 ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to read position in file stream: %s",
                   __FILE__, __LINE__, __func__, g_strerror(errno) );
      return false;
    }
    if( fseeko( stream, current_pos % CZI_ALIGNMENT, SEEK_CUR ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to seek position CUR+%li in file stream: %s",
                   __FILE__, __LINE__, __func__,
                   current_pos % CZI_ALIGNMENT, g_strerror(errno) );
      return false;
    }
/*
    // After 32 bytes alignment, we recheck that end was riched
    if( feof( stream ) )
      break;*/

    if( current_pos == previous_pos ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): We're not moving in file anymore. "
                   "Better break the loop and go to end of file. "
                   "At %li.",
                   __FILE__, __LINE__, __func__, ftello(stream) );
      fseeko( stream, 0, SEEK_END );
      return false;
    }

    //
    // read
    if( fread( (void*)id, sizeof(id[0]), sizeof(id), stream ) != sizeof(id) )
    {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to read %li items: %s",
                   __FILE__, __LINE__, __func__, sizeof(id),
                   g_strerror(errno) );
      return false;
    }
    if( !strcmp( id, CZI_FILE )      ||
        !strcmp( id, CZI_DIRECTORY ) ||
        !strcmp( id, CZI_SUBBLOCK )  ||
        !strcmp( id, CZI_METADATA )  ||
        !strcmp( id, CZI_ATTACH )    ||
        !strcmp( id, CZI_ATTDIR )    ||
        !strcmp( id, CZI_DELETED )
      )
    {
      strcpy( segmentheader->id, id );
      int64_t len;
      len = fread( (void*)&(segmentheader->allocated_size), sizeof(segmentheader->allocated_size), 1, stream );
      if( len != 1 )
      {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "(%s:%d:%s): Failed to read %li items (only %li): %s",
                     __FILE__, __LINE__, __func__,
                     len, sizeof(segmentheader->allocated_size),
                     g_strerror(errno) );
        return false;
      }
      len = fread( (void*)&(segmentheader->used_size), sizeof(segmentheader->used_size), 1, stream );
      if( len != 1 )
      {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "(%s:%d:%s): Failed to read %li items (only %li): %s",
                     __FILE__, __LINE__, __func__,
                     len, sizeof(segmentheader->used_size),
                     g_strerror(errno) );
        return false;
      }
      return true;
    }
    previous_pos = current_pos;
  }

  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "(%s:%d:%s): No segment left.",
               __FILE__, __LINE__, __func__ );
  return false;
}

bool czi_read_next_segment_header_with_id(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  const char                  * id,
  GError                     ** err
)
{
  g_assert( source );
  g_assert( source->stream );
  g_assert( segmentheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  FILE * stream = source->stream;

  while( !feof( stream ) )
  {
    if( !czi_read_next_segment_header( source, segmentheader, err ) ) {
      g_assert (err == NULL || *err != NULL);
      return false;
    }
    if( strcmp( id, segmentheader->id ) )
    {
      return true;
    }
    else
    {
      if( !czi_skip_segment( source, segmentheader, err ) ) {
        g_assert (err == NULL || *err != NULL);
        return false;
      }
    }
  }

  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "(%s:%d:%s): No segment %s found.",
               __FILE__, __LINE__, __func__, id );
  return false;
}

bool czi_skip_segment(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  GError                     ** err
)
{
  g_assert( source );
  g_assert( source->stream );
  g_assert( segmentheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  if( fseeko( source->stream, segmentheader->allocated_size, SEEK_CUR ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to seek position CUR+%li in file stream: %s",
                   __FILE__, __LINE__, __func__,
                   segmentheader->allocated_size, g_strerror(errno) );
      return false;
  }

  return true;
}

bool czi_is_zisraw( FILE * stream, GError ** err )
{
  //g_debug( "czi_is_zisraw" );
  g_assert( stream );
  int64_t pos = ftello( stream );

  char magic_string[16];
  if( !read_items( (void*)magic_string, 16, 1, stream, err ) ) {
    g_prefix_error( err, "Failed to read magic string: " );
    fseeko( stream, pos, SEEK_SET );
    return false;
  }

  if( fseeko( stream, pos, SEEK_SET ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to seek initial position" );
    return false;
  }

  if( strcmp( magic_string, CZI_FILE ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Not a ZISRAW stream" );
    return false;
  }

  return true;
}

//============================================================================
//   NEW
//============================================================================

struct _czi * czi_new( GError ** err )
{
  struct _czi * czi = (struct _czi*) g_slice_alloc0( sizeof(struct _czi) );
  if( !czi ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi) );
    return NULL;
  }

  czi->sources      = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_source );
  czi->file_headers = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_file_header );
  czi->rois         = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_roi );
  czi->levels       = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_level );
  czi->metadata     = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_metadata );
  //czi->attachments  = g_hash_table_new_full( (void(*)(gpointer)) &czi_free_attachment );
  czi->grids        = g_hash_table_new_full(
                        &g_int_hash,
                        &g_int_equal,
                        (void(*)(gpointer)) &czi_free_S32,
                        (void(*)(gpointer)) &_openslide_grid_destroy );
#ifdef CZI_DEBUG
  czi->tileread_counts = g_hash_table_new_full(
                        &g_int64_hash,
                        &g_int64_equal,
                        (void(*)(gpointer)) &czi_free_S64,
                        (void(*)(gpointer)) &czi_free_S64 );
  czi->tilecached_counts = g_hash_table_new_full(
                        &g_int64_hash,
                        &g_int64_equal,
                        (void(*)(gpointer)) &czi_free_S64,
                        (void(*)(gpointer)) &czi_free_S64 );
#endif

  if( !czi->sources  || ! czi->file_headers || !czi->levels ||
      !czi->rois || !czi->metadata /*|| !czi->attachments*/ ) {
    czi_free( czi );
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to initiate _czi structure" );
    return NULL;
  }

  return czi;
}

struct _czi_source * czi_new_source( GError ** err )
{
  struct _czi_source * source = (struct _czi_source*) g_slice_alloc0( sizeof(struct _czi_source) );
  if( !source ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_source) );
    return NULL;
  }
  return source;
}

struct _czi_level * czi_new_level( struct _czi * czi, GError ** err )
{
  g_assert( czi );

  struct _czi_level * level = (struct _czi_level*) g_slice_alloc0( sizeof(struct _czi_level) );
  if( !level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_level) );
    return NULL;
  }

  level->size = g_hash_table_new_full(
                  &g_str_hash,
                  &g_str_equal,
                  &g_free,
                  (void(*)(gpointer)) &czi_free_S32 );
  level->start = g_hash_table_new_full(
                  &g_str_hash,
                  &g_str_equal,
                  &g_free,
                  (void(*)(gpointer)) &czi_free_S32 );
  level->tiles = g_hash_table_new_full(
                  &g_int64_hash,
                  &g_int64_equal,
                  (void(*)(gpointer)) &czi_free_S64,
                  (void(*)(gpointer)) &czi_free_tile );

  if( !level->size  || !level->start || !level->tiles ) {
    czi_free_level( level );
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to initiate _czi_level structure" );
    return NULL;
  }

  level->czi = czi;
  return level;
}

struct _czi_file_header * czi_new_file_header( struct _czi * czi, GError ** err )
{
  g_assert( czi);

  struct _czi_file_header * file_header = (struct _czi_file_header*) g_slice_alloc0( sizeof(struct _czi_file_header) );
  if( !file_header ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_file_header) );
    return NULL;
  }
  file_header->czi = czi;
  return file_header;
}

struct _czi_roi * czi_new_roi( struct _czi * czi, GError ** err )
{
  g_assert( czi );

  struct _czi_roi * roi = (struct _czi_roi*) g_slice_alloc0( sizeof(struct _czi_roi) );
  if( !roi ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_roi) );
    return NULL;
  }
  roi->czi = czi;
  return roi;
}

struct _czi_metadata * czi_new_metadata( struct _czi * czi, GError ** err )
{
  g_assert( czi );

  struct _czi_metadata * metadata = (struct _czi_metadata*) g_slice_alloc0( sizeof(struct _czi_metadata) );
  if( !metadata ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_metadata) );
    return NULL;
  }
  metadata->czi = czi;
  return metadata;
}

struct _czi_attachment * czi_new_attachment(
  struct _czi * czi G_GNUC_UNUSED,
  GError ** err     G_GNUC_UNUSED
)
{
  /*TODO*/
  return NULL;
}

struct _czi_tile * czi_new_tile( GError ** err )
{
  struct _czi_tile * tile = (struct _czi_tile*) g_slice_alloc0( sizeof(struct _czi_tile) );
  if( !tile ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_tile) );
    return NULL;
  }

  tile->dimensions = g_hash_table_new_full( &g_str_hash, &g_str_equal,
                      &g_free, (void(*)(gpointer)) &czi_free_dimension );
  if( !tile->dimensions ) {
    g_slice_free( struct _czi_tile, tile );
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to initiate _czi_tile structure" );
    return NULL;
  }

  return tile;
}

struct _czi_dimension * czi_new_dimension( struct _czi_tile * tile, GError ** err )
{
  g_assert( tile );

  struct _czi_dimension * dimension = (struct _czi_dimension*) g_slice_alloc0( sizeof(struct _czi_dimension) );
  if( !dimension ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(struct _czi_dimension) );
    return NULL;
  }
  dimension->tile = tile;
  dimension->dimension_id[4] = '\0';
  return dimension;
}

int16_t * czi_new_S16( int16_t shortint, GError ** err )
{
  int16_t * value = (int16_t*) g_slice_alloc0( sizeof(int16_t) );
  if( !value ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(int16_t) );
    return NULL;
  }
  *value = shortint;
  return value;
}

int32_t * czi_new_S32( int32_t integer, GError ** err )
{
  int32_t * value = (int32_t*) g_slice_alloc0( sizeof(int32_t) );
  if( !value ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(int32_t) );
    return NULL;
  }
  *value = integer;
  return value;
}

int64_t * czi_new_S64( int64_t integer, GError ** err )
{
  int64_t * value = (int64_t*) g_slice_alloc0( sizeof(int64_t) );
  if( !value ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %ld bytes", sizeof(int64_t) );
    return NULL;
  }
  *value = integer;
  return value;
}


struct _openslide_czi_tile_descriptor * czi_new_tile_descriptor(
  struct _czi_tile  * tile,
  GError           ** err
)
{
  struct _czi_dimension * dim;
  struct _openslide_czi_tile_descriptor * tile_desc;

  tile_desc = (struct _openslide_czi_tile_descriptor *) g_slice_alloc0( sizeof(struct _openslide_czi_tile_descriptor) );
  tile_desc->uid = tile->uid;
  tile_desc->pixel_type = tile->pixel_type;
  tile_desc->compression = tile->compression;
  tile_desc->pyramid_type = tile->pyramid_type;

  dim = (struct _czi_dimension*) g_hash_table_lookup( tile->dimensions, "X" );
  if( !dim ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Tile without X dimension." );
    czi_free_tile_descriptor( tile_desc );
    return NULL;
  }
  tile_desc->subsampling_x = dim->size / dim->stored_size;
  tile_desc->size_x = dim->size;
  tile_desc->start_x = dim->start;

  dim = (struct _czi_dimension*) g_hash_table_lookup( tile->dimensions, "Y" );
  if( !dim ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Tile without Y dimension." );
    czi_free_tile_descriptor( tile_desc );
    return NULL;
  }
  tile_desc->subsampling_y = dim->size / dim->stored_size;
  tile_desc->size_y = dim->size;
  tile_desc->start_y = dim->start;

  return tile_desc;

}

struct _czi_pixel_dynamic_info * czi_new_pixel_dynamic_info(
                    enum czi_pixel_t pixel_type, 
                    GError ** err
) {
    struct _czi_pixel_dynamic_info * pdi;
    
    pdi = (struct _czi_pixel_dynamic_info *) g_slice_alloc0( 
              sizeof(struct _czi_pixel_dynamic_info)
          );
    
    enum _czi_data_t channel_type = czi_data_type(pixel_type);
    
    pdi->type = pixel_type;
    pdi->channel_count = _openslide_czi_pixel_type_channel_count(pixel_type);
    pdi->channel_size = czi_data_type_size(channel_type);
    
    // Set function to update dynamic information
    pdi->update = czi_pixel_dynamic_info_update;
    
    pdi->min_per_channel = g_slice_alloc0(pdi->channel_count
                                        * pdi->channel_size);
    
    pdi->max_per_channel = g_slice_alloc0(pdi->channel_count
                                        * pdi->channel_size);
    
    if (!pdi->update) {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Update function for minimum/maximum was not found."
                     "May pixel type is not supported yet." );
        czi_free_pixel_dynamic_info( pdi );
        return NULL;
    }
    
    return pdi;
}

struct _czi_accumulator * czi_new_accumulator(
                        enum _czi_accumulator_t accumulator_type,
                        enum _czi_data_t        data_type,
                        uint64_t                data_count,
                        GError **               err){
    struct _czi_accumulator * ca = (struct _czi_accumulator *) 
                                   g_slice_alloc0( 
                                       sizeof(struct _czi_accumulator)
                                   );
    const struct _czi_accumulator_func * caf = czi_get_accumulator_func(
                                                  accumulator_type,
                                                  data_type
                                               );
    if (caf) {
        // Point to the accumulate method of the static accumulator function
        // found
        ca->accumulate = caf->accumulate;
        ca->data_type = data_type;
        ca->data_type_size = czi_data_type_size(data_type);
        ca->data_count = data_count;
        
        ca->data = (uint8_t*)g_slice_alloc0(ca->data_type_size * data_count);
    }
    else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Unable to find accumulator function." );
        czi_free_accumulator(ca);
        return NULL;
    }
    
    return ca;
}

struct _czi_rescale_info * czi_new_rescale_info(
                        GError ** err                            G_GNUC_UNUSED){
    struct _czi_rescale_info * cri = (struct _czi_rescale_info *) 
                                     g_slice_alloc0( 
                                         sizeof(struct _czi_rescale_info)
                                     );
    cri->shift = 0;
    cri->slope = 1;
    
    return cri;
}

//============================================================================
//   FREE
//============================================================================

void czi_free( struct _czi * ptr )
{
  // g_debug( "czi_free" );
  if( ptr ) {
    if( ptr->sources )         g_ptr_array_free( ptr->sources, true );
    if( ptr->file_headers )    g_ptr_array_free( ptr->file_headers, true );
    if( ptr->levels )          g_ptr_array_free( ptr->levels, true );
    if( ptr->metadata )        g_ptr_array_free( ptr->metadata, true );
    if( ptr->rois )            g_ptr_array_free( ptr->rois, true );
    if( ptr->attachments )     g_hash_table_destroy( ptr->attachments );
    if( ptr->grids )           g_hash_table_destroy( ptr->grids );
#ifdef CZI_DEBUG
    if( ptr->tileread_counts ) {
        g_hash_table_destroy( ptr->tileread_counts );
    }
    if( ptr->tilecached_counts ) {
        g_hash_table_destroy( ptr->tilecached_counts );
    }
#endif
    g_slice_free( struct _czi, ptr );
  }
}

void czi_free_source( struct _czi_source * ptr )
{
  // g_debug( "czi_free_source" );
  if( ptr ) {
    if( ptr->filename ) g_free( ptr->filename );
    if( ptr->stream )   fclose( ptr->stream );
    g_slice_free( struct _czi_source, ptr );
  }
}

void czi_free_file_header( struct _czi_file_header * ptr )
{
  // g_debug( "czi_free_file_header" );
  if( ptr ) g_slice_free( struct _czi_file_header, ptr );
}

void czi_free_level( struct _czi_level * ptr )
{
  // g_debug( "czi_free_level" );
  if( ptr ) {
    if( ptr->size )       g_hash_table_destroy( ptr->size );
    if( ptr->start )      g_hash_table_destroy( ptr->start );
    if( ptr->tiles )      g_hash_table_destroy( ptr->tiles );
    g_slice_free( struct _czi_level, ptr );
  }
}

void czi_free_roi( struct _czi_roi * ptr )
{
  // g_debug( "czi_free_roi" );
  if( ptr ) {
    if( ptr->shapeparams )       g_hash_table_destroy( ptr->shapeparams );
    g_slice_free( struct _czi_roi, ptr );
  }
}

void czi_free_metadata( struct _czi_metadata * ptr )
{
  // g_debug( "czi_free_metadata" );
  if( ptr ) g_slice_free( struct _czi_metadata, ptr );
}

void czi_free_attachment( struct _czi_attachment * ptr )
{
  // g_debug(  " czi_free_attachment");
  if( ptr ) g_slice_free( struct _czi_attachment, ptr );
}

void czi_free_tile( struct _czi_tile * ptr )
{
  // g_debug( "czi_free_tile" );
  if( ptr ) {
    if( ptr->dimensions )  g_hash_table_destroy( ptr->dimensions );
    g_slice_free( struct _czi_tile, ptr );
  }
  return;
}

void czi_free_dimension( struct _czi_dimension * ptr )
{
  // g_debug( "czi_free_dimension" );
  if( ptr ) g_slice_free( struct _czi_dimension, ptr );
}

void czi_free_tile_descriptor( struct _openslide_czi_tile_descriptor * ptr )
{
  // g_debug( "czi_free_tile_descriptor" );
  if( ptr ) g_slice_free( struct _openslide_czi_tile_descriptor, ptr );
}

void czi_free_pixel_dynamic_info( struct _czi_pixel_dynamic_info * ptr )
{
  // g_debug( "czi_free_pixel_dynamic_info" );
  if( ptr ) {
      if(ptr->min_per_channel)
        g_slice_free1(ptr->channel_count
                    * ptr->channel_size,
                        ptr->min_per_channel);
      
      if(ptr->max_per_channel)
        g_slice_free1(ptr->channel_count
                    * ptr->channel_size,
                        ptr->max_per_channel);
      
      g_slice_free(struct _czi_pixel_dynamic_info, ptr);
  }
}

void czi_free_accumulator( struct _czi_accumulator * ptr )
{
  // g_debug( "czi_free_accumulator" );
  if( ptr ) {
      if (ptr->data)
          g_slice_free1(ptr->data_type_size * ptr->data_count,
                        ptr->data);

      g_slice_free(struct _czi_accumulator, ptr);
  }
}

void czi_free_rescale_info( struct _czi_rescale_info * ptr )
{
  // g_debug( "czi_free_rescale_info" );
  if( ptr ) {
      g_slice_free(struct _czi_rescale_info, ptr);
  }
}

void czi_free_S16( int16_t * ptr )
{
  // g_debug( "czi_free_S16" );
  if( ptr ) g_slice_free( int16_t, ptr );
}

void czi_free_S32( int32_t * ptr )
{
  // g_debug( "czi_free_S32" );
  if( ptr ) g_slice_free( int32_t, ptr );
}

void czi_free_S64( int64_t * ptr )
{
  // g_debug( "czi_free_S64" );
  if( ptr ) g_slice_free( int64_t, ptr );
}

//============================================================================
//   READ
//============================================================================

bool czi_parse_directory(
  struct _czi_source  * source,
  struct _czi         * czi,
  GError             ** err
)
{
  //g_debug( "czi_parse_directory" );
  g_assert( source );
  g_assert( source->stream );
  g_assert( czi );

  FILE * stream = source->stream;
  int32_t entry_count;
  int32_t ss_x, ss_y;
  struct _czi_tile * new_tile = NULL;
  struct _czi_dimension * dim;
  uint8_t uid[8];
  int32_t * ptr_S32;
  int64_t * ptr_S64;

  TRY_READ_ITEMS( &entry_count, 1, 4, stream, err, "Failed to parse directory: " );
  fseeko( stream, 124, SEEK_CUR );                       // 124 bytes reserved

  for( int32_t i=0; i<entry_count; ++i )
  {
    new_tile = czi_new_tile( err );
    if( !new_tile ) return false;
    if( !czi_read_tile( source, new_tile, err ) ) {
      czi_free_tile( new_tile );
      return false;
    }

    dim = (struct _czi_dimension*) g_hash_table_lookup( new_tile->dimensions, "X" );
    if( !dim ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Tile without X dimension." );
      czi_free_tile( new_tile );
      return false;
    }
    ss_x = dim->size / dim->stored_size;
    ptr_S32 = (int32_t*) (uid+4);
    *ptr_S32 = dim->start;
    //new_tile->uid = (uint64_t)( dim->start );

    dim = (struct _czi_dimension*) g_hash_table_lookup( new_tile->dimensions, "Y" );
    if( !dim ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Tile without Y dimension." );
      czi_free_tile( new_tile );
      return false;
    }
    ss_y = dim->size / dim->stored_size;
    ptr_S32 = (int32_t*) (uid);
    *ptr_S32 = dim->start;
    //new_tile->uid = (new_tile->uid) | ( (uint64_t)(dim->start) << 4 );

    ptr_S64 = (int64_t *) uid;
    new_tile->uid = *ptr_S64;
    if( !czi_add_tile( czi, new_tile, ss_x, ss_y, err ) ) {
      czi_free_tile( new_tile );
      return false;
    }
    new_tile = NULL;
  }

  g_ptr_array_sort(
    czi->levels,
    (gint(*)(gconstpointer,gconstpointer)) czi_cmp_level );
  return true;
}

bool czi_parse_attdir(
  struct _czi_source  * source G_GNUC_UNUSED,
  struct _czi         * czi    G_GNUC_UNUSED,
  GError             ** err    G_GNUC_UNUSED
)
{
  // TODO
  return true;
}

bool czi_read_file_header(
  struct _czi_source       * source,
  struct _czi_file_header  * file_header,
  GError                  ** err
)
{
  //g_debug ( "czi_read_file_header" );
  g_assert( source );
  g_assert( source->stream );
  g_assert( file_header );

  file_header->source = source;
  FILE * stream = source->stream;
  int32_t update_pending;

  TRY_READ_ITEMS( &(file_header->major),              1, 4, stream, err, "Failed to read file header: " );
  TRY_READ_ITEMS( &(file_header->minor),              1, 4, stream, err, "Failed to read file header: " );
  TRY_FSEEKO( stream, 8, SEEK_CUR, err, "Failed to read file header: " ); // 2 int reserved
  TRY_READ_ITEMS( file_header->primary_file_guid,    16, 1, stream, err, "Failed to read file header: " );
  TRY_READ_ITEMS( file_header->file_guid,            16, 1, stream, err, "Failed to read file header: " );
  TRY_READ_ITEMS( &(file_header->file_part),          1, 4, stream, err, "Failed to read file header: " );
  TRY_READ_ITEMS( &(file_header->directory_position), 1, 8, stream, err, "Failed to read file header: " );
  TRY_READ_ITEMS( &(file_header->metadata_position),  1, 8, stream, err, "Failed to read file header: " );
  TRY_READ_ITEMS( &(update_pending),                  1, 4, stream, err, "Failed to read file header: " );
  file_header->update_pending = (bool) update_pending;
  TRY_READ_ITEMS( &(file_header->attdir_position),    1, 8, stream, err, "Failed to read file header: " );

  return true;
}

bool czi_read_metadata(
  struct _czi_source    * source,
  struct _czi_metadata  * metadata,
  GError               ** err
)
{
  //g_debug( "czi_read_metadata" );
  g_assert( source );
  g_assert( source->stream );
  g_assert( metadata );

  metadata->source = source;
  FILE * stream = source->stream;

  TRY_READ_ITEMS( &(metadata->xml_size),        1, 4, stream, err, "Failed to read metadata: " );
  TRY_READ_ITEMS( &(metadata->attachment_size), 1, 4, stream, err, "Failed to read metadata: " );
  TRY_FSEEKO( stream, 248,                       SEEK_CUR, err, "Failed to read metadata: " );  // end of segment
  metadata->offset = (int64_t) ftello( stream );
  TRY_FSEEKO( stream, metadata->xml_size,        SEEK_CUR, err, "Failed to read metadata: " );  // skip xml block
  TRY_FSEEKO( stream, metadata->attachment_size, SEEK_CUR, err, "Failed to read metadata: " );  // skip att block
  return true;
}

bool czi_read_tile(
  struct _czi_source  * source,
  struct _czi_tile    * tile,
  GError             ** err
)
{
  // g_debug( "czi_read_tile" );
  g_assert( source );
  g_assert( source->stream );
  g_assert( tile );

  tile->source = source;
  FILE * stream = source->stream;
  int8_t  val8;
  int32_t val32;
  int32_t dimension_count;

  TRY_FSEEKO( stream, 2, SEEK_CUR, err, "Failed to read tile: " );                        // SchemaType
  TRY_READ_ITEMS( &val32,               1, 4, stream, err, "Failed to read tile: " );      // PixelType
  if( ( val32 >=0 && val32 <= 4 ) || ( val32 >=8 && val32 <= 13 ) )
    tile->pixel_type = (enum czi_pixel_t) val32;
  else
    tile->pixel_type = PXL_UNKNOWN;
  TRY_READ_ITEMS( &(tile->tile_offset), 1, 8, stream, err, "Failed to read tile: " );
  TRY_READ_ITEMS( &(tile->file_part),   1, 4, stream, err, "Failed to read tile: " );
  TRY_READ_ITEMS( &val32, 1, sizeof(int32_t), stream, err, "Failed to read tile: " );    // Compression

  if( val32 == 0 || val32 == 1 || val32 == 2 || val32 ==  4 )
    tile->compression = (enum czi_compression_t) val32;
  else if( val32 >= 100 && val32 < 1000 )
    tile->compression = CAMERA_SPEC;
  else if( val32 >= 1000 )
    tile->compression = SYSTEM_SPEC;
  else
    tile->compression = CMP_UNKNOWN;

  TRY_READ_ITEMS( &val8,                1, 1, stream, err, "Failed to read tile: " );    // PyramidType
  if( val8 >= 0 && val8 <= 2 )
    tile->pyramid_type = (enum czi_pyramid_t) val8;
  else
    tile->pyramid_type = PYR_UNKNOWN;
  TRY_FSEEKO( stream, 5, SEEK_CUR, err, "Failed to read tile: " );                          // Reserved
  TRY_READ_ITEMS( &dimension_count,     1, 4, stream, err, "Failed to read tile: " );

  for( int32_t i=0; i<dimension_count; ++i )
  {
    struct _czi_dimension * new_dimension = czi_new_dimension( tile, err );
    if( !czi_read_dimension( source, new_dimension, err ) ) {
      czi_free_tile( tile );
      return false;
    }

    g_hash_table_insert( tile->dimensions,
                         g_strndup( new_dimension->dimension_id, 1 ), new_dimension );
  }

  return true;
}

bool czi_read_dimension(
  struct _czi_source     * source,
  struct _czi_dimension  * dimension,
  GError                ** err
)
{
  //g_debug( "czi_read_dimension" );
  g_assert( source );
  g_assert( source->stream );
  g_assert( dimension );
  FILE * stream = source->stream;

  TRY_READ_ITEMS( dimension->dimension_id,        4, 1, stream, err, "Failed to read dimension: " );
  TRY_READ_ITEMS( &(dimension->start),            1, 4, stream, err, "Failed to read dimension: " );
  TRY_READ_ITEMS( &(dimension->size),             1, 4, stream, err, "Failed to read dimension: " );
  TRY_READ_ITEMS( &(dimension->start_coordinate), 1, 4, stream, err, "Failed to read dimension: " );
  TRY_READ_ITEMS( &(dimension->stored_size),      1, 4, stream, err, "Failed to read dimension: " );

  return true;
}

//============================================================================
//   DISPLAY
//============================================================================
#if 0
char * guid_to_string(
  uint8_t * guid
)
{
  char * str = (char*) g_malloc( 36 );
  int pos = 0;
  for( int i=0; i<4; ++i ) {
    sprintf( str+pos, "%X", guid[i] );
    pos += 2;
  }
  str[pos] = '-';
  pos++;
  for( int i=4; i<6; ++i ) {
    sprintf( str+pos, "%X", guid[i] );
    pos += 2;
  }
  str[pos] = '-';
  pos++;
  for( int i=6; i<8; ++i ) {
    sprintf( str+pos, "%X", guid[i] );
    pos += 2;
  }
  str[pos] = '-';
  pos++;
  for( int i=8; i<10; ++i ) {
    sprintf( str+pos, "%X", guid[i] );
    pos += 2;
  }
  str[pos] = '-';
  pos++;
  for( int i=10; i<16; ++i ) {
    sprintf( str+pos, "%X", guid[i] );
    pos += 2;
  }

  str[16] = '\0';
  return str;
}
#endif

void czi_display( struct _czi * ptr         G_GNUC_UNUSED,
                  uint16_t      alignment   G_GNUC_UNUSED )
{

}

void czi_display_source( struct _czi_source * ptr,
                         uint16_t             alignment )
{
  // g_debug( "czi_display_source" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ source:\n", alignment, "");
    if( ptr->filename ) fprintf( stdout, "%*s" "- filename: %s\n",
                                 alignment + CZI_DISPLAY_INDENT,
                                 "",
                                 ptr->filename );
    if( ptr->stream )   fprintf( stdout, "%*s" "- stream position: %jd\n",
                                 alignment + CZI_DISPLAY_INDENT,
                                 "",
                                 ftello( ptr->stream ) );
    fflush(stdout);
  }
}

void czi_display_segment_header( struct _czi_segment_header * ptr,
                                 uint16_t                     alignment )
{
  // g_debug( "czi_display_segment_header" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ segment header:\n", alignment, "");
    fprintf( stdout, "%*s" "- id: %.*s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     16,
                     ptr->id );
    fprintf( stdout, "%*s" "- allocated size: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->allocated_size );
    fprintf( stdout, "%*s" "- used size: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->used_size );
    fflush(stdout);
  }
}

void czi_display_file_header( struct _czi_file_header * ptr,
                              uint16_t                  alignment )
{
  // g_debug( "czi_display_file_header" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ file header:\n", alignment, "");
    fprintf( stdout, "%*s" "- version: %d.%d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->major,
                     ptr->minor );
    fprintf( stdout, "%*s" "- primary file guid: %.*s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     16, ptr->primary_file_guid );
    fprintf( stdout, "%*s" "- file guid: %.*s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     16, ptr->file_guid );
    fprintf( stdout, "%*s" "- file part: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->file_part );
    fprintf( stdout, "%*s" "- directory position: %zu\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->directory_position );
    fprintf( stdout, "%*s" "- metadata position: %zu\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->metadata_position );
    fprintf( stdout, "%*s" "- update pending: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ( ptr->update_pending ? "true" : "false" ) );
    fprintf( stdout, "%*s" "- attachment directory position: %zu\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->attdir_position );
    fflush(stdout);
  }
}

void czi_display_roi( struct _czi_roi * ptr,
                      uint16_t          alignment )
{
  // g_debug( "czi_display_roi" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ roi:\n", alignment, "");
    fprintf( stdout, "%*s" "- type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_roi_shape_t_string(ptr->shape) );
    fprintf( stdout, "%*s" "- covering_mode: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_roi_covering_mode_t_string(ptr->covering_mode) );
    fprintf( stdout, "%*s" "- overlap: %lf\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->overlap);
    fprintf( stdout, "%*s" "- x: %lf\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->x);
    fprintf( stdout, "%*s" "- y: %lf\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->y);
    fprintf( stdout, "%*s" "- w: %lf\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->w);
    fprintf( stdout, "%*s" "- h: %lf\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->h);
    fprintf( stdout, "%*s" "- rows: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->rows );
    fprintf( stdout, "%*s" "- columns: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->columns );
    fflush(stdout);
  }
}

void czi_display_level( struct _czi_level * ptr,
                        uint16_t alignment )
{
  // g_debug( "czi_display_level" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ level:\n", alignment, "");
    fprintf( stdout, "%*s" "- pixel_type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_pixel_t_string(ptr->pixel_type) );
    fprintf( stdout, "%*s" "- compression: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_compression_t_string(ptr->compression) );
    fprintf( stdout, "%*s" "- pyramid_type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_pyramid_t_string(ptr->pyramid_type) );
    fprintf( stdout, "%*s" "- subsampling_x: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->subsampling_x);
    fprintf( stdout, "%*s" "- subsampling_y: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->subsampling_y);
    fflush(stdout);
  }
}

void czi_display_metadata( struct _czi_metadata * ptr,
                           uint16_t alignment )
{
  // g_debug( "czi_display_metadata" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ metadata:\n", alignment, "");
    fprintf( stdout, "%*s" "- offset: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->offset);
    fprintf( stdout, "%*s" "- xml_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->xml_size);
    fprintf( stdout, "%*s" "- attachment_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->attachment_size);
    fflush(stdout);
  }
}

void czi_display_attachment( struct _czi_attachment * ptr,
                             uint16_t alignment )
{
  // g_debug(  "czi_display_attachment");
  if( ptr ) {
    fprintf( stdout, "%*s" "+ attachement:\n", alignment, "");
    fprintf( stdout, "%*s" "- file_position: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->file_position);
    fprintf( stdout, "%*s" "- file_part: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->file_part);
    fprintf( stdout, "%*s" "- content_guid: %.*s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     5,
                     ptr->content_guid );
    fprintf( stdout, "%*s" "- content_file_type: %.*s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     8,
                     ptr->content_file_type );
    fprintf( stdout, "%*s" "- name: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->name );
    fprintf( stdout, "%*s" "- data_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->data_size);
    fflush(stdout);
  }
}

void czi_display_tile( struct _czi_tile * ptr,
                       uint16_t alignment )
{
  // g_debug( "czi_display_tile" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ tile:\n", alignment, "");
    fprintf( stdout, "%*s" "- file_part: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->file_part);
    fprintf( stdout, "%*s" "- tile_offset: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->tile_offset);
    fprintf( stdout, "%*s" "- uid: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->uid);
    fprintf( stdout, "%*s" "- pixel_type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_pixel_t_string(ptr->pixel_type) );
    fprintf( stdout, "%*s" "- compression: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_compression_t_string(ptr->compression) );
    fprintf( stdout, "%*s" "- pyramid_type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_pyramid_t_string(ptr->pyramid_type) );
    fprintf( stdout, "%*s" "- directory_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->directory_size);
    fprintf( stdout, "%*s" "- metadata_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->metadata_size);
    fprintf( stdout, "%*s" "- data_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->data_size);
    fprintf( stdout, "%*s" "- attachment_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->attachment_size);
    fflush(stdout);
  }
}

void czi_display_dimension( struct _czi_dimension * ptr,
                            uint16_t alignment )
{
  // g_debug( "czi_display_dimension" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ dimension:\n", alignment, "");
    fprintf( stdout, "%*s" "- dimension_id: %.*s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     5,
                     ptr->dimension_id );
    fprintf( stdout, "%*s" "- start: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->start);
    fprintf( stdout, "%*s" "- size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->size);
    fprintf( stdout, "%*s" "- start_coordinate: %f\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->start_coordinate);
    fprintf( stdout, "%*s" "- stored_size: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->stored_size);
    fflush(stdout);

  }
}

void czi_display_tile_descriptor( struct _openslide_czi_tile_descriptor * ptr,
                                  uint16_t alignment )
{
  //g_debug( "czi_display_tile_descriptor" );
  if( ptr ) {
    fprintf( stdout, "%*s" "+ tile_descriptor:\n", alignment, "");
    fprintf( stdout, "%*s" "- uid: %ld\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->uid);
    fprintf( stdout, "%*s" "- pixel_type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_pixel_t_string( ptr->pixel_type ) );
    fprintf( stdout, "%*s" "- compression: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_compression_t_string( ptr->compression ) );
    fprintf( stdout, "%*s" "- pyramid_type: %s\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     czi_pyramid_t_string( ptr->pyramid_type ) );
    fprintf( stdout, "%*s" "- subsampling_x: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->subsampling_x);
    fprintf( stdout, "%*s" "- subsampling_y: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->subsampling_y);
    fprintf( stdout, "%*s" "- start_x: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->start_x);
    fprintf( stdout, "%*s" "- start_y: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->start_y);
    fprintf( stdout, "%*s" "- size_x: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->size_x);
    fprintf( stdout, "%*s" "- size_y: %d\n",
                     alignment + CZI_DISPLAY_INDENT,
                     "",
                     ptr->size_y);
    fflush(stdout);
  }
}


//============================================================================
//   STRING CONVERSION
//============================================================================
const char * czi_pixel_t_string( enum czi_pixel_t        pixel_type ) {
  switch(pixel_type) {
    default:
    case PXL_UNKNOWN:
      return pxl_unknown;

    case GRAY_8:
      return gray_8;

    case GRAY_16:
      return gray_16;

    case GRAY_32_FLOAT:
      return gray_32_float;

    case BGR_24:
      return bgr_24;

    case BGR_48:
      return bgr_48;

    case BGR_96_FLOAT:
      return bgr_96_float;

    case BGRA_32:
      return bgra_32;

    case GRAY_64_COMPLEX_FLOAT:
      return gray_64_complex_float;

    case BGR_192_COMPLEX_FLOAT:
      return bgr_192_complex_float;

    case GRAY_32:
      return gray_32;

    case GRAY_64:
      return gray_64;
  }
}

const char * czi_compression_t_string( enum czi_compression_t  compression_type ) {
  switch(compression_type) {
    default:
    case CMP_UNKNOWN:
      return pxl_unknown;

    case UNCOMPRESSED:
      return uncompressed;

    case JPEG:
      return jpeg;

    case LZW:
      return lzw;

    case JPEGXR:
      return jpegxr;

    case CAMERA_SPEC:
      return camera_spec;

    case SYSTEM_SPEC:
      return system_spec;
  }
}

const char * czi_pyramid_t_string( enum czi_pyramid_t      pyramid_type ) {
  switch(pyramid_type) {
    default:
    case PYR_UNKNOWN:
      return pyr_unknown;

    case NONE:
      return none;

    case SINGLE:
      return single;

    case MULTI:
      return multi;
  }
}

const char * czi_roi_shape_t_string( enum czi_roi_shape_t    roi_shape_type ) {
  switch(roi_shape_type) {
    default:
    case SHP_UNKNOWN:
      return shp_unknown;

    case ELLIPSE:
      return ellipse;

    case RECTANGLE:
      return rectangle;

    case POLYGON:
      return polygon;
  }
}

const char * czi_roi_covering_mode_t_string( enum czi_roi_covering_mode_t roi_covering_mode ) {
  switch(roi_covering_mode) {
    default:
    case COV_UNKNOWN:
      return cov_unknown;

    case ALIGNED_TO_GLOBAL_GRID:
      return aligned_to_global_grid;

    case ALIGNED_TO_LOCAL_TILE_REGION:
      return aligned_to_local_tile_region;
  }
}

const char * czi_boolean_t_string( bool b ) {
    return (b ? "true" : "false");
}

// key:PARSING-PRI-DEF
//============================================================================
//
//                  ZISRAW PARSING PRIVATE API: DEFINITIONS
//
//============================================================================


//============================================================================
//   PRIVATE STRUCTURE DEFINITIONS
//============================================================================
//--- accumulator functions ----------------------------------------------------
//--- minimum ------------------------------------------------------------------
const struct _czi_accumulator_func _czi_accumulator_min_U8 = {
  .accumulate = czi_accumulator_min_accumulate_U8,
};

const struct _czi_accumulator_func _czi_accumulator_min_U16 = {
  .accumulate = czi_accumulator_min_accumulate_U16,
};

const struct _czi_accumulator_func _czi_accumulator_min_U32 = {
  .accumulate = czi_accumulator_min_accumulate_U32,
};

const struct _czi_accumulator_func _czi_accumulator_min_U64 = {
  .accumulate = czi_accumulator_min_accumulate_U64,
};

const struct _czi_accumulator_func _czi_accumulator_min_FLOAT = {
  .accumulate = czi_accumulator_min_accumulate_FLOAT,
};

const struct _czi_accumulator_func _czi_accumulator_min_CFLOAT = {
  .accumulate = czi_accumulator_min_accumulate_CFLOAT,
};

//--- maximum ------------------------------------------------------------------
const struct _czi_accumulator_func _czi_accumulator_max_U8 = {
  .accumulate = czi_accumulator_max_accumulate_U8,
};

const struct _czi_accumulator_func _czi_accumulator_max_U16 = {
  .accumulate = czi_accumulator_max_accumulate_U16,
};

const struct _czi_accumulator_func _czi_accumulator_max_U32 = {
  .accumulate = czi_accumulator_max_accumulate_U32,
};

const struct _czi_accumulator_func _czi_accumulator_max_U64 = {
  .accumulate = czi_accumulator_max_accumulate_U64,
};

const struct _czi_accumulator_func _czi_accumulator_max_FLOAT = {
  .accumulate = czi_accumulator_max_accumulate_FLOAT,
};

const struct _czi_accumulator_func _czi_accumulator_max_CFLOAT = {
  .accumulate = czi_accumulator_max_accumulate_CFLOAT,
};

//--- rescale info -------------------------------------------------------------
const struct _czi_rescale_info_func _czi_rescale_info_U16_to_U8 = {
  .rescale_info = czi_rescale_info_U16_to_U8,
};

const struct _czi_rescale_info_func _czi_rescale_info_FLOAT_to_U8 = {
  .rescale_info = czi_rescale_info_FLOAT_to_U8,
};

//--- channel converter --------------------------------------------------------
const struct _czi_channel_converter _czi_channel_converter_U8_to_U8 = {
  .convert = czi_channel_convert_U8_to_U8,
  .src_channel_type = U8_TYPE,
  .dest_channel_type = U8_TYPE,
};

const struct _czi_channel_converter _czi_channel_converter_U16_to_U8 = {
  .convert = czi_channel_convert_U16_to_U8,
  .src_channel_type = U16_TYPE,
  .dest_channel_type = U8_TYPE,
};

const struct _czi_channel_converter _czi_channel_converter_U32_to_U8 = {
  .convert = czi_channel_convert_U32_to_U8,
  .src_channel_type = U32_TYPE,
  .dest_channel_type = U8_TYPE,
};

const struct _czi_channel_converter _czi_channel_converter_U64_to_U8 = {
  .convert = czi_channel_convert_U64_to_U8,
  .src_channel_type = U64_TYPE,
  .dest_channel_type = U8_TYPE,
};

const struct _czi_channel_converter _czi_channel_converter_FLOAT_to_U8 = {
  .convert = czi_channel_convert_FLOAT_to_U8,
  .src_channel_type = FLOAT_TYPE,
  .dest_channel_type = U8_TYPE,
};

const struct _czi_channel_converter _czi_channel_converter_CFLOAT_to_U8 = {
  .convert = czi_channel_convert_CFLOAT_to_U8,
  .src_channel_type = CFLOAT_TYPE,
  .dest_channel_type = U8_TYPE,
};

//--- pixel converter ----------------------------------------------------------
const struct _czi_pixel_converter _czi_pixel_converter_BGR_24_to_BGRA_32 = {
  .convert = czi_pixel_convert_BGR_24_to_BGRA_32,
  .src_pixel_type = BGR_24,
  .dest_pixel_type = BGRA_32,
};

const struct _czi_pixel_converter _czi_pixel_converter_BGRA_32_to_BGRA_32 = {
  .convert = czi_pixel_convert_BGRA_32_to_BGRA_32,
  .src_pixel_type = BGRA_32,
  .dest_pixel_type = BGRA_32,
};

const struct _czi_pixel_converter _czi_pixel_converter_BGR_48_to_BGRA_32 = {
  .convert = czi_pixel_convert_BGR_48_to_BGRA_32,
  .src_pixel_type = BGR_48,
  .dest_pixel_type = BGRA_32,
};

const struct _czi_pixel_converter _czi_pixel_converter_BGR_96_FLOAT_to_BGRA_32 = {
  .convert = czi_pixel_convert_BGR_96_FLOAT_to_BGRA_32,
  .src_pixel_type = BGR_96_FLOAT,
  .dest_pixel_type = BGRA_32,
};

const struct _czi_pixel_converter _czi_pixel_converter_BGR_192_COMPLEX_FLOAT_to_BGRA_32 = {
  .convert = czi_pixel_convert_BGR_192_COMPLEX_FLOAT_to_BGRA_32,
  .src_pixel_type = BGR_192_COMPLEX_FLOAT,
  .dest_pixel_type = BGRA_32,
};

//==============================================================================
//   PRIVATE METHODS DEFINITIONS
//==============================================================================
//--- converters ---------------------------------------------------------------
int32_t czi_uid_S32(uint8_t                b0,
                    uint8_t                b1,
                    uint8_t                b2,
                    uint8_t                b3) {
   int32_t  uid;
   ((uint8_t*)&uid)[0] = b0;
   ((uint8_t*)&uid)[1] = b1;
   ((uint8_t*)&uid)[2] = b2;
   ((uint8_t*)&uid)[3] = b3;
   
   return uid;
}

//--- buffer convert -----------------------------------------------------------
void czi_buffer_convert_U8_to_U8(
                         uint8_t                              * src_buffer,
                         uint8_t                              * dest_buffer,
                         double                                 shift,
                         double                                 scale
) {
    (*dest_buffer) = (uint8_t)((*((uint8_t*)src_buffer) + shift) * scale);
}

void czi_buffer_convert_U16_to_U8(
                         uint8_t                              * src_buffer,
                         uint8_t                              * dest_buffer,
                         double                                 shift,
                         double                                 scale
) {
    /*
    g_debug("value:%d, scaled value: %d",
        *((uint16_t*)src_buffer),
        (uint8_t)((*((uint16_t*)src_buffer) + shift) * scale)
    );*/
    (*dest_buffer) = (uint8_t)((*((uint16_t*)src_buffer) + shift) * scale);
}

void czi_buffer_convert_U32_to_U8(
                         uint8_t                              * src_buffer,
                         uint8_t                              * dest_buffer,
                         double                                 shift,
                         double                                 scale
) {
    (*dest_buffer) = (uint8_t)((*((uint32_t*)src_buffer) + shift) * scale);
}

void czi_buffer_convert_U64_to_U8(
                         uint8_t                              * src_buffer,
                         uint8_t                              * dest_buffer,
                         double                                 shift,
                         double                                 scale
) {
    (*dest_buffer) = (uint8_t)((*((uint64_t*)src_buffer) + shift) * scale);
}

void czi_buffer_convert_FLOAT_to_U8(
                         uint8_t                              * src_buffer,
                         uint8_t                              * dest_buffer,
                         double                                 shift,
                         double                                 scale
) {
    (*dest_buffer) = (uint8_t)((*((float*)src_buffer) + shift) * scale);
}

void czi_buffer_convert_CFLOAT_to_U8(
                         uint8_t                              * src_buffer,
                         uint8_t                              * dest_buffer,
                         double                                 shift,
                         double                                 scale
) {
    (*dest_buffer) = (uint8_t)((cabsf(*((float complex *)src_buffer)) + shift) * scale);
}

//--- typed convert ------------------------------------------------------------
uint8_t czi_convert_U8_to_U8(
                         uint8_t                                value,
                         double                                 shift,
                         double                                 scale
) {
    uint8_t result;
    czi_buffer_convert_U8_to_U8(&value,
                                &result,
                                shift,
                                scale);
    return result;
}

uint8_t czi_convert_U16_to_U8(
                         uint16_t                               value,
                         double                                 shift,
                         double                                 scale
) {
    uint8_t result;
    czi_buffer_convert_U16_to_U8((uint8_t*)&value,
                                 &result,
                                 shift,
                                 scale);
    return result;
}

uint8_t czi_convert_U32_to_U8(
                         uint32_t                               value,
                         double                                 shift,
                         double                                 scale
) {
    uint8_t result;
    czi_buffer_convert_U32_to_U8((uint8_t*)&value,
                                 &result,
                                 shift,
                                 scale);
    return result;
}

uint8_t czi_convert_U64_to_U8(
                         uint64_t                               value,
                         double                                 shift,
                         double                                 scale
) {
    uint8_t result;
    czi_buffer_convert_U64_to_U8((uint8_t*)&value,
                                 &result,
                                 shift,
                                 scale);
    return result;
}

uint8_t czi_convert_FLOAT_to_U8(
                         float                                  value,
                         double                                 shift,
                         double                                 scale
) {
    uint8_t result;
    czi_buffer_convert_U64_to_U8((uint8_t*)&value,
                                 &result,
                                 shift,
                                 scale);
    return result;
}

uint8_t czi_convert_CFLOAT_to_U8(
                         complex float                          value,
                         double                                 shift,
                         double                                 scale
) {
    uint8_t result;
    czi_buffer_convert_U64_to_U8((uint8_t*)&value,
                                 &result,
                                 shift,
                                 scale);
    return result;
}

//--- data type ----------------------------------------------------------------
uint8_t czi_data_type_size( enum _czi_data_t type ) {
    switch(type) {
        default:
        case DATA_TYPE_UNKNOWN:
            return 0;
            
        case U8_TYPE:
            return 1;
            
        case U16_TYPE:
            return 2;
            
        case U32_TYPE:
        case FLOAT_TYPE:
            return 4;
            
        case U64_TYPE:
        case CFLOAT_TYPE:
            return 8;
    }
}

enum _czi_data_t czi_data_type( enum czi_pixel_t type ) {

    switch(type) {
        default:
        case PXL_UNKNOWN:
            return DATA_TYPE_UNKNOWN;
            
        case GRAY_8:
        case BGR_24:
        case BGRA_32:
            return U8_TYPE;

        case GRAY_16:
        case BGR_48:
            return U16_TYPE;

        case GRAY_32_FLOAT:
        case BGR_96_FLOAT:
            return FLOAT_TYPE;

        case GRAY_64_COMPLEX_FLOAT:
        case BGR_192_COMPLEX_FLOAT:
            return CFLOAT_TYPE;

        case GRAY_32:
            return U32_TYPE;

        case GRAY_64:
            return U64_TYPE;
    }
}

//--- accumulator functions ----------------------------------------------------
G_LOCK_DEFINE(_czi_accumulator_func_hash_table);
GHashTable * _czi_accumulator_func_hash_table = NULL;

int32_t czi_accumulator_func_uid(
                        enum _czi_accumulator_t                type,
                        enum _czi_data_t                       data_type) { 
   return czi_uid_S32((uint8_t)type, 0, (uint8_t)data_type, 0);
}

GHashTable * czi_accumulator_func_hash_table(
                        GError                              ** err) {
    if (!_czi_accumulator_func_hash_table) {
        G_LOCK(_czi_accumulator_func_hash_table);
        // g_debug("czi_accumulator_func_hash_table");
        _czi_accumulator_func_hash_table = g_hash_table_new_full(
                    &g_int_hash,
                    &g_int_equal,
                    (void(*)(gpointer)) &czi_free_S32, 
                    (void(*)(gpointer)) NULL // No need to free accumulator 
                                             // functions as they are allocated
                                             // statically
        );
        
        // Insert each accumulator function into _czi_accumulator_func_hash_table
        //--- minimum functions ------------------------------------------------
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MIN_ACCUMULATOR, U8_TYPE),
                err),
            (gpointer)&_czi_accumulator_min_U8);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MIN_ACCUMULATOR, U16_TYPE),
                err),
            (gpointer)&_czi_accumulator_min_U16);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MIN_ACCUMULATOR, U32_TYPE),
                err),
            (gpointer)&_czi_accumulator_min_U32);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MIN_ACCUMULATOR, U64_TYPE),
                err),
            (gpointer)&_czi_accumulator_min_U64);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MIN_ACCUMULATOR, FLOAT_TYPE),
                err),
            (gpointer)&_czi_accumulator_min_FLOAT);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MIN_ACCUMULATOR, CFLOAT_TYPE),
                err),
            (gpointer)&_czi_accumulator_min_CFLOAT);      
        
        //--- maximum functions ------------------------------------------------
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MAX_ACCUMULATOR, U8_TYPE),
                err),
            (gpointer)&_czi_accumulator_max_U8);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MAX_ACCUMULATOR, U16_TYPE),
                err),
            (gpointer)&_czi_accumulator_max_U16);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MAX_ACCUMULATOR, U32_TYPE),
                err),
            (gpointer)&_czi_accumulator_max_U32);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MAX_ACCUMULATOR, U64_TYPE),
                err),
            (gpointer)&_czi_accumulator_max_U64);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MAX_ACCUMULATOR, FLOAT_TYPE),
                err),
            (gpointer)&_czi_accumulator_max_FLOAT);
        
        g_hash_table_insert(
            _czi_accumulator_func_hash_table,
            czi_new_S32(
                czi_accumulator_func_uid(MAX_ACCUMULATOR, CFLOAT_TYPE),
                err),
            (gpointer)&_czi_accumulator_max_CFLOAT);
        
        G_UNLOCK(_czi_accumulator_func_hash_table);
    }
    return _czi_accumulator_func_hash_table;
}

const struct _czi_accumulator_func * czi_get_accumulator_func( 
                                  enum _czi_accumulator_t      type,
                                  enum _czi_data_t             data_type
) {
     GHashTable * accumulator_func = czi_accumulator_func_hash_table(0);
     
     return (const struct _czi_accumulator_func *)
            g_hash_table_lookup(
                accumulator_func,
                czi_new_S32(
                    czi_accumulator_func_uid(type,
                                             data_type),
                    0
                )
            );
}

//--- accumulator --------------------------------------------------------------
void czi_buffer_accumulate(
                            enum _czi_accumulator_t   accumulator_type,
                            uint8_t                 * buffer,
                            uint8_t                 * result,
                            enum _czi_data_t          data_type,
                            uint64_t                  data_count,
                            GError                 ** err){   
    if (data_count > 0) {
        uint8_t data_type_size = czi_data_type_size(data_type);
        
        struct _czi_accumulator * accumulator = czi_new_accumulator(
                                                    accumulator_type,
                                                    data_type,
                                                    1,
                                                    err
                                                );
        
        // Initializes accumulator
        if (accumulator) {
            memcpy(accumulator->data, 
                   buffer,
                   data_type_size);
        }
        else {
            g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                        "Unable to allocate accumulator." );
            return;
        }
        
        // Go through values in the buffer and accumulate data
        uint8_t * p = buffer + data_type_size;
        for (uint64_t i = 1; (i < data_count); ++i) {
            accumulator->accumulate(accumulator, 0, p);
            p += data_type_size;
        }
        
        // Copy result
        memcpy(result, 
               accumulator->data,
               data_type_size);    
        
        czi_free_accumulator(accumulator);
    }
    /*
    else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unable to initialize accumulator because buffer does not "
                    "contain data.");
    }
    */
}


//--- channel converters -------------------------------------------------------
// Hash table of channel converters
G_LOCK_DEFINE(_czi_channel_converter_hash_table);
GHashTable * _czi_channel_converter_hash_table = NULL;

int32_t czi_channel_converter_uid(
                                  enum _czi_data_t               src_type,
                                  enum _czi_data_t               dest_type) {
   
   return czi_uid_S32(0, (uint8_t)dest_type, 0, (uint8_t)src_type);
}

GHashTable * czi_channel_converter_hash_table(GError         ** err) {
    
    if (!_czi_channel_converter_hash_table){
        g_debug("czi_channel_converter_hash_table::initialize channel converter hash table");
        G_LOCK(_czi_channel_converter_hash_table);
        _czi_channel_converter_hash_table = g_hash_table_new_full(
                    &g_int_hash,
                    &g_int_equal,
                    (void(*)(gpointer)) &czi_free_S32, 
                    (void(*)(gpointer)) NULL // No need to free converters as they
                                            // are allocated statically
        );
        
        // Insert each convert into converters g_hash_table
        g_hash_table_insert(
            _czi_channel_converter_hash_table,
            czi_new_S32(
                czi_channel_converter_uid(U8_TYPE,
                                          U8_TYPE),
                err
            ),
            (gpointer)&_czi_channel_converter_U8_to_U8
        );

        g_hash_table_insert(
            _czi_channel_converter_hash_table,
            czi_new_S32(
                czi_channel_converter_uid(U16_TYPE,
                                          U8_TYPE),
                err
            ),
            (gpointer)&_czi_channel_converter_U16_to_U8
        );
        
        g_hash_table_insert(
            _czi_channel_converter_hash_table,
            czi_new_S32(
                czi_channel_converter_uid(U32_TYPE,
                                          U8_TYPE),
                err
            ),
            (gpointer)&_czi_channel_converter_U32_to_U8
        );
        
        g_hash_table_insert(
            _czi_channel_converter_hash_table,
            czi_new_S32(
                czi_channel_converter_uid(U64_TYPE,
                                          U8_TYPE),
                err
            ),
            (gpointer)&_czi_channel_converter_U64_to_U8
        );
        
        g_hash_table_insert(
            _czi_channel_converter_hash_table,
            czi_new_S32(
                czi_channel_converter_uid(FLOAT_TYPE,
                                          U8_TYPE),
                err
            ),
            (gpointer)&_czi_channel_converter_FLOAT_to_U8
        );
        
        g_hash_table_insert(
            _czi_channel_converter_hash_table,
            czi_new_S32(
                czi_channel_converter_uid(CFLOAT_TYPE,
                                          U8_TYPE),
                err
            ),
            (gpointer)&_czi_channel_converter_CFLOAT_to_U8
        );
        
        G_UNLOCK(_czi_channel_converter_hash_table);
    }
    return _czi_channel_converter_hash_table;
}

const struct _czi_channel_converter * czi_get_channel_converter( 
    enum _czi_data_t src_channel_type,
    enum _czi_data_t dest_channel_type
) {
     GHashTable * converters = czi_channel_converter_hash_table(0);
     
     return (const struct _czi_channel_converter *)
            g_hash_table_lookup(
                converters,
                czi_new_S32(
                    czi_channel_converter_uid(src_channel_type,
                                              dest_channel_type),
                    0
                )
            );
}

void czi_channel_convert_U8_to_U8(
                        const struct _czi_channel_converter  * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err           G_GNUC_UNUSED
) {
    czi_buffer_convert_U8_to_U8(src_buffer, 
                                dest_buffer, 
                                (cri ? cri->shift : 0),
                                (cri ? cri->slope : 1));
}

void czi_channel_convert_U16_to_U8(
                        const struct _czi_channel_converter  * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err           G_GNUC_UNUSED
) {
    //g_debug("czi_channel_convert_U16_to_U8");
    czi_buffer_convert_U16_to_U8(src_buffer, 
                                 dest_buffer, 
                                 (cri ? cri->shift : 0),
                                 (cri ? cri->slope : 1));
}

void czi_channel_convert_U32_to_U8(
                        const struct _czi_channel_converter  * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err           G_GNUC_UNUSED
) {
    czi_buffer_convert_U32_to_U8(src_buffer, 
                                 dest_buffer, 
                                 (cri ? cri->shift : 0),
                                 (cri ? cri->slope : 1));
}

void czi_channel_convert_U64_to_U8(
                        const struct _czi_channel_converter  * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err           G_GNUC_UNUSED
) {
    czi_buffer_convert_U64_to_U8(src_buffer, 
                                 dest_buffer, 
                                 (cri ? cri->shift : 0),
                                 (cri ? cri->slope : 1));
}

void czi_channel_convert_FLOAT_to_U8(
                        const struct _czi_channel_converter  * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err           G_GNUC_UNUSED
) {
    czi_buffer_convert_FLOAT_to_U8(src_buffer, 
                                   dest_buffer, 
                                   (cri ? cri->shift : 0),
                                   (cri ? cri->slope : 1));
}

void czi_channel_convert_CFLOAT_to_U8(
                        const struct _czi_channel_converter  * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err           G_GNUC_UNUSED
) {
    czi_buffer_convert_CFLOAT_to_U8(src_buffer, 
                                    dest_buffer, 
                                    (cri ? cri->shift : 0),
                                    (cri ? cri->slope : 1));
}

//--- pixel dynamic info -------------------------------------------------------

void czi_pixel_dynamic_info_update(
                        struct _czi_pixel_dynamic_info * pdi,
                        uint8_t                        * buffer,
                        uint64_t                         buffer_size,
                        GError                        ** err ){
    
    struct _czi_accumulator * min_accumulator = czi_new_accumulator(
                                            MIN_ACCUMULATOR,
                                            pdi->type,
                                            (uint64_t)pdi->channel_count,
                                            err
                                        );
    struct _czi_accumulator * max_accumulator = czi_new_accumulator(
                                            MAX_ACCUMULATOR,
                                            pdi->type,
                                            (uint64_t)pdi->channel_count,
                                            err
                                        );
    // Initializes accumulators
    if (min_accumulator) {
        memcpy(min_accumulator->data, 
               buffer,
               pdi->channel_count * pdi->channel_size);
    }
    else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unable to allocate min accumulator." );
        return;
    }
    
    if (max_accumulator) {    
        memcpy(max_accumulator->data, 
               buffer,
               pdi->channel_count * pdi->channel_size);
    }
    else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unable to allocate max accumulator." );
        return;
    }
    
    // Go through data and update min/max information for each pixel
    uint64_t p = pdi->channel_count * pdi->channel_size;
    while (p < buffer_size) {
        for (uint8_t c = 0; (c < pdi->channel_count); ++c) {
            min_accumulator->accumulate(min_accumulator, c, buffer + p);
            max_accumulator->accumulate(max_accumulator, c, buffer + p);
            p += pdi->channel_size;
        }
    }
    
    //g_debug("Copy min/max accumulator data to pdi");
    memcpy(pdi->min_per_channel, 
           min_accumulator->data,
           pdi->channel_count * pdi->channel_size);
    
    memcpy(pdi->max_per_channel,
           max_accumulator->data,
           pdi->channel_count * pdi->channel_size);

    czi_free_accumulator(min_accumulator);
    czi_free_accumulator(max_accumulator);
}

/*
void czi_pixel_dynamic_info_update_min_max_multichannel(
                        struct _czi_pixel_dynamic_info * pdi,
                        uint8_t                        * buffer){
    if (pdi->min_accumulator && pdi->max_accumulator) {
        for (uint8_t c = 0; (c < pdi->channel_count); ++c) {
            pdi->min_accumulator->accumulate(pdi->min_accumulator, c, buffer);
            pdi->max_accumulator->accumulate(pdi->max_accumulator, c, buffer);
            buffer += pdi->channel_size;
        }
    }
    else{
        g_debug("Unable to update pixel dynamic information because"
                "either min_accumulator is NULL or max_accumulator is NULL."
        );
    }
}
*/
//---minimum accumulators-------------------------------------------------------
void czi_accumulator_min_accumulate_U8(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update minimum
    if (*(((uint8_t *)ac->data) + pos) > *((uint8_t *)buffer))
        *(((uint8_t *)ac->data) + pos) = *((uint8_t *)buffer);
}

void czi_accumulator_min_accumulate_U16(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update minimum
    if (*(((uint16_t *)ac->data) + pos) > *((uint16_t *)buffer))
        *(((uint16_t *)ac->data) + pos) = *((uint16_t *)buffer);
}

void czi_accumulator_min_accumulate_U32(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update minimum
    if (*(((uint32_t *)ac->data) + pos) > *((uint32_t *)buffer))
        *(((uint32_t *)ac->data) + pos) = *((uint32_t *)buffer);
}

void czi_accumulator_min_accumulate_U64(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update minimum
    if (*(((uint64_t *)ac->data) + pos) > *((uint64_t *)buffer))
        *(((uint64_t *)ac->data) + pos) = *((uint64_t *)buffer);
}

void czi_accumulator_min_accumulate_FLOAT(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update minimum
    if (*(((float *)ac->data) + pos) > *((float *)buffer))
        *(((float *)ac->data) + pos) = *((float *)buffer);
}

void czi_accumulator_min_accumulate_CFLOAT(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update minimum
    if (cabsf(*(((complex float *)ac->data) + pos)) > cabsf(*((complex float *)buffer)))
        *(((complex float *)ac->data) + pos) = *((complex float *)buffer);
}

//---maximum accumulators-------------------------------------------------------
void czi_accumulator_max_accumulate_U8(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update maximum
    if (*(((uint8_t *)ac->data) + pos) < *((uint8_t *)buffer))
        *(((uint8_t *)ac->data) + pos) = *((uint8_t *)buffer);
}

void czi_accumulator_max_accumulate_U16(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update maximum
    if (*(((uint16_t *)ac->data) + pos) < *((uint16_t *)buffer))
        *(((uint16_t *)ac->data) + pos) = *((uint16_t *)buffer);
}

void czi_accumulator_max_accumulate_U32(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update maximum
    if (*(((uint32_t *)ac->data) + pos) < *((uint32_t *)buffer))
        *(((uint32_t *)ac->data) + pos) = *((uint32_t *)buffer);
}

void czi_accumulator_max_accumulate_U64(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update maximum
    if (*(((uint64_t *)ac->data) + pos) < *((uint64_t *)buffer))
        *(((uint64_t *)ac->data) + pos) = *((uint64_t *)buffer);
}

void czi_accumulator_max_accumulate_FLOAT(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update maximum
    if (*(((float *)ac->data) + pos) < *((float *)buffer))
        *(((float *)ac->data) + pos) = *((float *)buffer);
}

void czi_accumulator_max_accumulate_CFLOAT(
                        struct _czi_accumulator        * ac,
                        uint64_t                         pos,
                        uint8_t                        * buffer
) {
    // Update maximum
    if (cabsf(*(((complex float *)ac->data) + pos)) < cabsf(*((complex float *)buffer)))
        *(((complex float *)ac->data) + pos) = *((complex float *)buffer);
}

//--- rescale info -------------------------------------------------------------
G_LOCK_DEFINE(_czi_rescale_info_func_hash_table);
GHashTable * _czi_rescale_info_func_hash_table = NULL;

uint32_t czi_rescale_info_func_uid(
                                    enum _czi_data_t                  src_type,
                                    enum _czi_data_t                  dest_type) {
   return czi_uid_S32(0, (uint8_t)dest_type, 0, (uint8_t)src_type);
}

GHashTable * czi_rescale_info_func_hash_table(GError           ** err) {
    
    if (!_czi_rescale_info_func_hash_table) {
        // g_debug("czi_rescale_info_func_hash_table");
        G_LOCK(_czi_rescale_info_func_hash_table);
         _czi_rescale_info_func_hash_table = g_hash_table_new_full(
                    &g_int_hash,
                    &g_int_equal,
                    (void(*)(gpointer)) &czi_free_S32, 
                    (void(*)(gpointer)) NULL // No need to free converters as they
                                            // are allocated statically
        );

        // Insert each rescale_info function into g_hash_table
        g_hash_table_insert(
            _czi_rescale_info_func_hash_table,
            czi_new_S32(
                czi_rescale_info_func_uid(U16_TYPE, U8_TYPE),
                err),
            (gpointer)&_czi_rescale_info_U16_to_U8
        );
        
        g_hash_table_insert(
            _czi_rescale_info_func_hash_table,
            czi_new_S32(
                czi_rescale_info_func_uid(FLOAT_TYPE, U8_TYPE),
                err),
            (gpointer)&_czi_rescale_info_FLOAT_to_U8
        );
        G_UNLOCK(_czi_rescale_info_func_hash_table);
    }
    return _czi_rescale_info_func_hash_table;
}

const struct _czi_rescale_info_func * czi_get_rescale_info_func( 
                                    enum _czi_data_t                  src_type,
                                    enum _czi_data_t                  dest_type) {
     GHashTable * rescale_info_func_hash_table = czi_rescale_info_func_hash_table(0);
     
     return (const struct _czi_rescale_info_func *)g_hash_table_lookup(
                                    rescale_info_func_hash_table,
                                    czi_new_S32(
                                        czi_rescale_info_func_uid(src_type,
                                                                  dest_type),
                                        0)
                                  );
}

struct _czi_rescale_info * czi_rescale_info_U16_to_U8(
                                    struct _czi_pixel_dynamic_info  * pdi,
                                    GError                         ** err) {

    struct _czi_rescale_info * cri = czi_new_rescale_info(0);
                          
    uint16_t min_value = USHRT_MAX, max_value = 0;
        
    // for (uint8_t c = 0; c < pdi->channel_count; ++c)
    //     g_debug("czi_rescale_info_U16_to_U8:: min_per_channel[%d] %d, max_per_channel[%d] %d",
    //             c, *(((uint16_t*)pdi->min_per_channel) + c),
    //             c, *(((uint16_t*)pdi->max_per_channel) + c));
                
    czi_buffer_accumulate(MIN_ACCUMULATOR,
                          pdi->min_per_channel,
                          (uint8_t *)&min_value,
                          U16_TYPE,
                          pdi->channel_count,
                          err);
    czi_buffer_accumulate(MAX_ACCUMULATOR,
                          pdi->max_per_channel,
                          (uint8_t *)&max_value,
                          U16_TYPE,
                          pdi->channel_count,
                          err);
    //g_debug("czi_rescale_info_U16_to_U8, min_value: %d, max_value: %d",
    //        min_value, max_value
    //);
    if ((max_value - min_value) >= UCHAR_MAX) {
        //g_debug("czi_rescale_info_U16_to_U8, need to rescale dynamic");
        // Need to rescale dynamic
        if (min_value > 0) {
            // If the dynamic minimum value is not 0 it may be important
            // to keep the particular significance of the 0 value. So we 
            // calculate slope using 255 values and the output dynamic is 
            // processed to start at value 1.
            cri->slope = UCHAR_MAX / ((double)(max_value - min_value + 1));
            cri->shift = (1 / cri->slope) - min_value;
        }
        else {
            cri->slope = (UCHAR_MAX + 1) / ((double)(max_value + 1));
            cri->shift = (-(double)min_value);            
        }
    }
    else if (max_value > UCHAR_MAX) {
        //g_debug("czi_rescale_info_U16_to_U8, need to shift dynamic");
        // Shift the dynamic to keep data in the destination range
        cri->shift = UCHAR_MAX - (double)max_value;
        cri->slope = 1;
    }
    else {
        //g_debug("czi_rescale_info_U16_to_U8, keep default");
        // Default is to keep dynamic shift and to have a slope of 1
        cri->shift = 0;
        cri->slope = 1;
    }
        
    return cri;
}

struct _czi_rescale_info * czi_rescale_info_FLOAT_to_U8(
                                    struct _czi_pixel_dynamic_info  * pdi,
                                    GError                         ** err) {

    struct _czi_rescale_info * cri = czi_new_rescale_info(0);
                          
    float min_value = FLT_MAX, max_value = 0;
        
    // for (uint8_t c = 0; c < pdi->channel_count; ++c)
    //     g_debug("czi_rescale_info_U16_to_U8:: min_per_channel[%d] %d, max_per_channel[%d] %d",
    //             c, *(((uint16_t*)pdi->min_per_channel) + c),
    //             c, *(((uint16_t*)pdi->max_per_channel) + c));
                
    czi_buffer_accumulate(MIN_ACCUMULATOR,
                          pdi->min_per_channel,
                          (uint8_t *)&min_value,
                          FLOAT_TYPE,
                          pdi->channel_count,
                          err);
    czi_buffer_accumulate(MAX_ACCUMULATOR,
                          pdi->max_per_channel,
                          (uint8_t *)&max_value,
                          FLOAT_TYPE,
                          pdi->channel_count,
                          err);
    //g_debug("czi_rescale_info_U16_to_U8, min_value: %d, max_value: %d",
    //        min_value, max_value
    //);
    if ((max_value - min_value) >= UCHAR_MAX) {
        //g_debug("czi_rescale_info_U16_to_U8, need to rescale dynamic");
        // Need to rescale dynamic
        if (min_value > 0) {
            // If the dynamic minimum value is not 0 it may be important
            // to keep the particular significance of the 0 value. So we 
            // calculate slope using 255 values and the output dynamic is 
            // processed to start at value 1.
            cri->slope = UCHAR_MAX / ((double)(max_value - min_value + 1));
            cri->shift = (1 / cri->slope) - min_value;
        }
        else {
            cri->slope = (UCHAR_MAX + 1) / ((double)(max_value + 1));
            cri->shift = (-(double)min_value);            
        }
    }
    else if (max_value > UCHAR_MAX) {
        //g_debug("czi_rescale_info_U16_to_U8, need to shift dynamic");
        // Shift the dynamic to keep data in the destination range
        cri->shift = UCHAR_MAX - (double)max_value;
        cri->slope = 1;
    }
    else {
        //g_debug("czi_rescale_info_U16_to_U8, keep default");
        // Default is to keep dynamic shift and to have a slope of 1
        cri->shift = 0;
        cri->slope = 1;
    }
        
    return cri;
}

//--- pixel converters ---------------------------------------------------------
G_LOCK_DEFINE(_czi_converter_hash_table);
GHashTable * _czi_converter_hash_table = NULL;

uint32_t czi_pixel_converter_uid(
                                  enum czi_pixel_t                  src_type,
                                  enum czi_pixel_t                  dest_type) {
   return czi_uid_S32(0, (uint8_t)dest_type, 0, (uint8_t)src_type);
}

GHashTable * czi_pixel_converter_hash_table(GError           ** err) {
    
    if (!_czi_converter_hash_table) {
        //g_debug("czi_new_pixel_converter_hash_table");
        G_LOCK(_czi_converter_hash_table);
         _czi_converter_hash_table = g_hash_table_new_full(
                    &g_int_hash,
                    &g_int_equal,
                    (void(*)(gpointer)) &czi_free_S32, 
                    (void(*)(gpointer)) NULL // No need to free converters as they
                                            // are allocated statically
        );

        // Insert each convert into converters g_hash_table
        g_hash_table_insert(
            _czi_converter_hash_table,
            czi_new_S32(
                czi_pixel_converter_uid(BGR_24, BGRA_32),
                err),
            (gpointer)&_czi_pixel_converter_BGR_24_to_BGRA_32
        );
        
        g_hash_table_insert(
            _czi_converter_hash_table,
            czi_new_S32(
                czi_pixel_converter_uid(BGRA_32, BGRA_32),
                err),
            (gpointer)&_czi_pixel_converter_BGRA_32_to_BGRA_32
        );
        
        g_hash_table_insert(
            _czi_converter_hash_table,
            czi_new_S32(
                czi_pixel_converter_uid(BGR_48, BGRA_32),
                err),
            (gpointer)&_czi_pixel_converter_BGR_48_to_BGRA_32
        );
        
        g_hash_table_insert(
            _czi_converter_hash_table,
            czi_new_S32(
                czi_pixel_converter_uid(BGR_96_FLOAT, BGRA_32),
                err),
            (gpointer)&_czi_pixel_converter_BGR_96_FLOAT_to_BGRA_32
        );
        
        g_hash_table_insert(
            _czi_converter_hash_table,
            czi_new_S32(
                czi_pixel_converter_uid(BGR_192_COMPLEX_FLOAT, BGRA_32),
                err),
            (gpointer)&_czi_pixel_converter_BGR_192_COMPLEX_FLOAT_to_BGRA_32
        );
        
        G_UNLOCK(_czi_converter_hash_table);
    }
    return _czi_converter_hash_table;
}

const struct _czi_pixel_converter * czi_get_pixel_converter( 
    enum czi_pixel_t src_pixel_type,
    enum czi_pixel_t dest_pixel_type
) {
     GHashTable * converters = czi_pixel_converter_hash_table(0);
     
     return (const struct _czi_pixel_converter *)g_hash_table_lookup(
                                    converters,
                                    czi_new_S32(
                                        czi_pixel_converter_uid(src_pixel_type,
                                                                dest_pixel_type),
                                        0)
                                  );
}

void czi_pixel_convert_multichannel(
                        const struct _czi_channel_converter  * cpc,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        uint8_t                                channel_count,
                        GError                              ** err) {
    uint8_t src_channel_size = czi_data_type_size(cpc->src_channel_type),
            dest_channel_size = czi_data_type_size(cpc->dest_channel_type);
    for (uint8_t c = 0; (c < channel_count); ++c) {
        cpc->convert(cpc, cri, src_buffer, dest_buffer, err);
        src_buffer += src_channel_size;
        dest_buffer += dest_channel_size;
    }
}

void czi_pixel_convert_BGR_24_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err) {   
    czi_pixel_convert_multichannel(
        &_czi_channel_converter_U8_to_U8,
        cri,
        src_buffer, 
        dest_buffer,
        _openslide_czi_pixel_type_channel_count(cpc->src_pixel_type),
        err
    );
    
    *(dest_buffer + 3) = 255;
}

void czi_pixel_convert_BGRA_32_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err) {
    czi_pixel_convert_multichannel(
        &_czi_channel_converter_U8_to_U8,
        cri,
        src_buffer, 
        dest_buffer,
        _openslide_czi_pixel_type_channel_count(cpc->src_pixel_type),
        err
    );
}

void czi_pixel_convert_BGR_48_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err) {
    /*
     g_debug("czi_pixel_convert_BGR_48_to_BGRA_32:: src_buffer %x, dest_buffer %x, channel count %d", 
             src_buffer,
             dest_buffer,
             _openslide_czi_pixel_type_channel_count(cpc->src_pixel_type));
     g_debug("czi_pixel_convert_BGR_48_to_BGRA_32:: pixel values: %d, %d, %d", 
             *((uint16_t*)(src_buffer)), 
             *((uint16_t*)(src_buffer + 1)), 
             *((uint16_t*)(src_buffer + 2)));
             */
    czi_pixel_convert_multichannel(
        &_czi_channel_converter_U16_to_U8,
        cri,
        src_buffer, 
        dest_buffer,
        _openslide_czi_pixel_type_channel_count(cpc->src_pixel_type),
        err
    );
    
    // Test 1 - BGRA: is the default
//#define TEST_BRGA
//#define TEST_RGBA
//#define TEST_RBGA
//#define TEST_GBRA
//#define TEST_GRBA
    
#ifdef TEST_BRGA
    // Test 2 - BRGA: R<=>G
    *(dest_buffer + 3) = *(dest_buffer + 2);
    *(dest_buffer + 2) = *(dest_buffer + 1);
    *(dest_buffer + 1) = *(dest_buffer + 3);
#endif
    
#ifdef TEST_RGBA
    // Test 3 - RGBA: R<=>B
    *(dest_buffer + 3) = *(dest_buffer);
    *(dest_buffer) = *(dest_buffer + 2);
    *(dest_buffer + 2) = *(dest_buffer + 3);
#endif

#ifdef TEST_RBGA    
    // Test 4 - RBGA: G<=>R R<=>B
    *(dest_buffer + 3) = *(dest_buffer + 2);
    *(dest_buffer + 2) = *(dest_buffer + 1);
    *(dest_buffer + 1) = *(dest_buffer + 3);
    
    *(dest_buffer + 3) = *(dest_buffer);
    *(dest_buffer) = *(dest_buffer + 1);
    *(dest_buffer + 1) = *(dest_buffer + 3);
#endif
    
#ifdef TEST_GBRA
    // Test 5 - GBRA: G<=>B
    *(dest_buffer + 3) = *(dest_buffer);
    *(dest_buffer) = *(dest_buffer + 1);
    *(dest_buffer + 1) = *(dest_buffer + 3);
#endif
    
#ifdef TEST_GRBA
    // Test 6 - GRBA: G<=>B R<=>B  
    *(dest_buffer + 3) = *(dest_buffer);
    *(dest_buffer) = *(dest_buffer + 1);
    *(dest_buffer + 1) = *(dest_buffer + 3);
    
    *(dest_buffer + 3) = *(dest_buffer + 2);
    *(dest_buffer + 2) = *(dest_buffer + 1);
    *(dest_buffer + 1) = *(dest_buffer + 3);
#endif    
    *(dest_buffer + 3) = 255;
    
    /*
     g_debug("czi_pixel_convert_BGR_48_to_BGRA_32:: converted pixel values: %d, %d, %d, %d", 
             *(dest_buffer), 
             *(dest_buffer + 1), 
             *(dest_buffer + 2),
             *(dest_buffer + 3));    
    */
}

void czi_pixel_convert_BGR_96_FLOAT_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err) {
    czi_pixel_convert_multichannel(
        &_czi_channel_converter_FLOAT_to_U8,
        cri,
        src_buffer, 
        dest_buffer,
        _openslide_czi_pixel_type_channel_count(cpc->src_pixel_type),
        err
    );
    
    *(dest_buffer + 3) = 255;
}

void czi_pixel_convert_BGR_192_COMPLEX_FLOAT_to_BGRA_32(
                        const struct _czi_pixel_converter    * cpc           G_GNUC_UNUSED,
                        const struct _czi_rescale_info       * cri,
                        uint8_t                              * src_buffer,
                        uint8_t                              * dest_buffer,
                        GError                              ** err) {
    czi_pixel_convert_multichannel(
        &_czi_channel_converter_CFLOAT_to_U8,
        cri,
        src_buffer, 
        dest_buffer,
        _openslide_czi_pixel_type_channel_count(cpc->src_pixel_type),
        err
    );
    
    *(dest_buffer + 3) = 255;
}

//--- check ------------------------------------------------------------------
bool _openslide_czi_is_zisraw( const char * filename, GError ** err )
{
  //g_debug( "_openslide_czi_is_zisraw" );
  g_assert( filename );

  FILE * stream = _openslide_fopen( filename, "rb", err );
  if( !stream )
    return false;

  if( czi_is_zisraw( stream, err ) ) {
    fclose( stream );
    return true;
  } else {
    fclose( stream );
    return false;
  }
}

//--- decode -----------------------------------------------------------------
_openslide_czi * _openslide_czi_decode( const char * filename, GError ** err )
{
  //g_debug( "_openslide_czi_decode" );
  g_assert( filename );

  _openslide_czi * czi = czi_new( err );
  if( !czi )
    return NULL;

  // TODO: look for multiple files
  // While not done: register one file
  if( !czi_find_sources( filename, czi, err ) ) {
    czi_free( czi );
    return NULL;
  }

  // decode each file
  for( uint32_t i=0; i<czi->sources->len; ++i ) {
    if( !czi_decode_one_stream( g_ptr_array_index( czi->sources, i ), czi, err ) ) {
      czi_free( czi );
      return NULL;
    }
  }

  return czi;
}

//--- tiles ------------------------------------------------------------------

uint8_t _openslide_czi_pixel_type_size( enum czi_pixel_t type ) {
  switch(type) {
    case GRAY_8:
      return 1;

    case GRAY_16:
      return 2;

    case BGR_24:
      return 3;

    case GRAY_32:
    case GRAY_32_FLOAT:
    case BGRA_32:
      return 4;

    case BGR_48:
      return 6;

    case GRAY_64:
    case GRAY_64_COMPLEX_FLOAT:
      return 8;

    case BGR_96_FLOAT:
      return 12;

    case BGR_192_COMPLEX_FLOAT:
      return 24;

    default:
      return 0;
  }
}


uint8_t _openslide_czi_pixel_type_channel_count( enum czi_pixel_t type ) {
  switch(type) {
    case GRAY_8:
    case GRAY_16:
    case GRAY_32:
    case GRAY_32_FLOAT:
    case GRAY_64:
    case GRAY_64_COMPLEX_FLOAT:
      return 1;

    case BGR_24:
    case BGR_48:
    case BGR_96_FLOAT:
    case BGR_192_COMPLEX_FLOAT:
      return 3;

    case BGRA_32:
      return 4;

    default:
      return 0;
  }
}

int32_t _openslide_czi_get_roi_count( _openslide_czi  * czi )
{
  return czi->rois->len;
}

int32_t _openslide_czi_get_level_count( _openslide_czi  * czi )
{
  return czi->levels->len;
}

int32_t _openslide_czi_get_level_subsampling(
  _openslide_czi  * czi,
  int32_t           level,
  GError         ** err
)
{
  struct _czi_level * s_level = (struct _czi_level *) g_ptr_array_index( czi->levels, level );
  if( !s_level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return 0;
  }
  return s_level->subsampling_x;
}

bool _openslide_czi_get_level_tile_size(
  _openslide_czi         * czi,
  int32_t                  level,
  int32_t                * w,
  int32_t                * h,
  GError                ** err
)
{
  struct _czi_level * s_level = (struct _czi_level *) g_ptr_array_index( czi->levels, level );
  if( !s_level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return false;
  }
  GList * keys = g_hash_table_get_keys( s_level->tiles );
  if( !keys ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "No key in level %d", level );
    return false;
  }
  g_list_free( keys );

  // Goes through level tiles to find maximum width and height
  *w = 0;
  *h = 0;

  struct _czi_tile  * tile;
  GList * current_tile = g_hash_table_get_values( s_level->tiles );

  while( current_tile )
  {
    tile = (struct _czi_tile *) current_tile->data;
    if( !tile ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Failed to load tile from level %d", level );
      g_list_free( current_tile );
      return false;
    }
    else {
      struct _czi_dimension * dim;

      dim = (struct _czi_dimension *) g_hash_table_lookup( tile->dimensions, "X" );
      if( !dim ) {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Failed to load X dimension from level %d", level );
        g_list_free( current_tile );
        return false;
      }
      *w = MAX(dim->stored_size, *w);

      dim = (struct _czi_dimension *) g_hash_table_lookup( tile->dimensions, "Y" );
      if( !dim ) {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Failed to load Y dimension from level %d", level );
        g_list_free( current_tile );
        return false;
      }
      *h = MAX(dim->stored_size, *h);
    }
    current_tile = g_list_next( current_tile );
  }

  g_list_free( current_tile );

  return true;
}

bool _openslide_czi_get_level_tile_offset(
  _openslide_czi         * czi,
  int32_t                  level,
  int32_t                * x,
  int32_t                * y,
  GError                ** err
)
{
  //g_debug("_openslide_czi_get_level_tile_offset:: level: %d, level_count: %d", level, czi->levels->len);
  
  if (level >= (int32_t)czi->levels->len) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return false;
  }
    
  struct _czi_level * s_level = g_ptr_array_index( czi->levels, level );
  if( !s_level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return false;
  }

  int32_t level_tiles_count = g_hash_table_size(s_level->tiles);
  if( level_tiles_count <= 0 ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "No tiles in level %d", level );
    return false;
  }

  *x = *(int32_t *) g_hash_table_lookup( s_level->start, "X" );
//   g_debug("_openslide_czi_get_level_tile_offset:: s_level->start_x: %d", *x);
  if( !x ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to load X start from level %d", level );
    return false;
  }
  *y = *(int32_t *) g_hash_table_lookup( s_level->start, "Y" );
//   g_debug("_openslide_czi_get_level_tile_offset:: s_level->start_y: %d", *y);
  if( !y ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to load Y start from level %d", level );
    return false;
  }

  return true;
}

struct _czi_tile * _openslide_czi_get_level_tile( _openslide_czi * czi, int32_t level, int64_t uid, GError **err )
{
    // Get czi level
  struct _czi_level * s_level = g_ptr_array_index( czi->levels, level );
  if( !s_level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return NULL;
  }

  // Get czi tile
  struct _czi_tile * tile = (struct _czi_tile *) g_hash_table_lookup( s_level->tiles, &uid );
  if (!tile) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find tile %ld", uid );
    return NULL;
  }

  return tile;
}

uint8_t * _openslide_czi_get_level_tile_data( _openslide_czi * czi, int32_t level, int64_t uid, int32_t * buffer_size, GError **err )
{
  //   g_debug("_openslide_czi_get_level_tile_offset:: level: %d, level_count: %d", level, czi->levels->len);

  // Get czi tile
  struct _czi_tile * tile = _openslide_czi_get_level_tile( czi, level, uid, err);
  if( !tile ) {
    (*buffer_size) = 0;
    return NULL;
  }

  if(!(tile->data_buf)) {
    // Try to load data for tile
    _openslide_czi_load_tile(czi, level, uid, buffer_size, err);
  }
  else {
    // Data was already loaded for tile
    if( buffer_size ) (*buffer_size) = tile->data_size;
  }

  return tile->data_buf;
}

bool _openslide_czi_free_level_tile_data( _openslide_czi * czi, int32_t level, int64_t uid, GError **err )
{
  // Get czi tile
  struct _czi_tile * tile = _openslide_czi_get_level_tile( czi, level, uid, err);
  if( !tile ) {
    return false;
  }

  if (tile->data_buf) {
    //g_debug( "_openslide_czi_free_level_tile_data:: level: %d, uid: %ld", level, uid );
    g_slice_free1( tile->data_size, tile->data_buf );
    tile->data_buf = NULL;
  }
  else {
    return false;
  }

  return true;
}

GList * _openslide_czi_get_level_tiles(
  _openslide_czi  * czi,
  int32_t           i,
  GError         ** err
)
{
  // g_debug("_openslide_czi_get_level_tiles");
  struct _czi_level * level;
  struct _czi_tile * tile;
  struct _openslide_czi_tile_descriptor * tile_desc;
  GList * intern_list, * intern_list_current, * extern_list = NULL;

  level = (struct _czi_level *) g_ptr_array_index( czi->levels, i );
  intern_list = g_hash_table_get_values( level->tiles );
  intern_list_current = intern_list;
  while( intern_list_current )
  {
    tile = (struct _czi_tile *) intern_list_current->data;
    tile_desc = czi_new_tile_descriptor( tile, err );
    if( !tile_desc ) {
      _openslide_czi_free_list_tiles( extern_list );
      return NULL;
    }

    extern_list = g_list_insert_before( extern_list, extern_list, tile_desc );
    intern_list_current = g_list_next( intern_list_current );
  }
  g_list_free( intern_list );

  return extern_list;
}

uint8_t * _openslide_czi_data_convert_to_rgba32(
  enum czi_pixel_t            pixel_type,
  uint8_t                   * tile_data,
  int32_t                     tile_data_size,
  int32_t                   * converted_tile_data_size,
  GError                   ** err
)
{
  //g_debug("_openslide_czi_data_convert_to_rgba32");
  const int8_t output_pixel_type_size = 4;

  if (!tile_data) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Unable to convert tile data to rgba32" );
    //g_debug( "_openslide_czi_data_convert_to_rgba32::tile_data is NULL" );
    return NULL;
  }

  // TODO: Add support for czi data types coded using more than 4 bytes
  int8_t czi_pixel_type_size = _openslide_czi_pixel_type_size( pixel_type );
  // g_debug("_openslide_czi_data_convert_to_rgba32:: pixel type size %d", czi_pixel_type_size);
  if ((!czi_pixel_type_size)/* || (czi_pixel_type_size > 4)*/) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Unable to convert tile data to rgba32" );
    return NULL;
  }

  (*converted_tile_data_size) = tile_data_size * output_pixel_type_size / czi_pixel_type_size;
  uint8_t * converted_tile_data = g_slice_alloc0( *converted_tile_data_size );
  if (!converted_tile_data) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Unable to allocate converted tile data" );
    return NULL;
  }

  // g_debug( "_openslide_czi_data_convert_to_rgba32::tile_data_size: %d"
  //          ", converted_tile_data_size: %d"
  //          ", output_pixel_type_size: %d"
  //          ", czi_pixel_type_size: %d",
  //          tile_data_size, *converted_tile_data_size, output_pixel_type_size, czi_pixel_type_size );
  const struct _czi_pixel_converter * converter = czi_get_pixel_converter(
                                                      pixel_type,
                                                      BGRA_32
                                                  );
  if (!converter) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Unable to convert tile data to rgba" );
    return NULL;
  }

  struct _czi_rescale_info * ri = NULL;
  const struct _czi_rescale_info_func * rif = czi_get_rescale_info_func(
                                                  czi_data_type(pixel_type), 
                                                  U8_TYPE
                                              );
  if (rif) {
    g_debug("Rescaling dynamic while converting %s to %s",
            czi_pixel_t_string(pixel_type),
            czi_pixel_t_string(BGRA_32));
    // In cases where precision is lost we try to resize data the best using
    // an automatically pre calculated slope.
    struct _czi_pixel_dynamic_info * pdi = czi_new_pixel_dynamic_info(pixel_type, err);
    pdi->update(pdi, tile_data, tile_data_size, err);    
    ri = rif->rescale_info(pdi, err);
    g_debug("Rescale using shift %lf and slope %lf", 
            ri->shift,
            ri->slope);    
  }
  
  for (uint32_t i = 0, j = 0;
       i < (uint32_t)tile_data_size;
       i += czi_pixel_type_size,
       j += output_pixel_type_size) {
//      g_debug("tile_data + %ld: %x, converted_tile_data + %ld: %x", 
//              i, (uint8_t *) (tile_data + i),
//              j, (uint8_t *) (converted_tile_data + j));
//    g_debug("pixel values: %d, %d, %d", 
//            *(((uint16_t*)tile_data) + i), 
//            *(((uint16_t*)tile_data) + i + 1), 
//            *(((uint16_t*)tile_data) + i + 2));
    converter->convert(converter,
                       ri,
                       (uint8_t *)(tile_data + i),
                       (uint8_t *)(converted_tile_data + j),
                       err);
//    g_debug("converted pixel values: %d, %d, %d, %d", 
//             *(converted_tile_data + j), 
//             *(converted_tile_data + j + 1), 
//             *(converted_tile_data + j + 2),
//             *(converted_tile_data + j + 3));    
    
  }

  czi_free_rescale_info(ri);

  return converted_tile_data;
}

uint8_t * _openslide_czi_uncompress( const struct _openslide_czi_uncompressor * uncompressor,
                                     void *data, int32_t data_size,
                                     int32_t width, int32_t height,
                                     enum czi_pixel_t pixel_type,
                                     int32_t * uncompressed_data_size,
                                     GError ** err )
{
  if (!uncompressor) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Invalid uncompressor" );
    *uncompressed_data_size = 0;
    return NULL;
  }

  // Allocate destination buffer
  *uncompressed_data_size = width
                           * height
                           * _openslide_czi_pixel_type_size(pixel_type);
  uint8_t * dest = g_slice_alloc0( *uncompressed_data_size );

  // Uncompress data to destination buffer
  if ( !uncompressor->uncompress( data, (uint32_t)data_size,
                                  (uint32_t *)dest,
                                  width,
                                  height,
                                  err ) ) {
    g_slice_free1( *uncompressed_data_size,
                   dest );
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to uncompress tile data using uncompressor %s",
                 uncompressor->name );
    *uncompressed_data_size = 0;
    return NULL;
  }

  return dest;
}

uint8_t * _openslide_czi_uncompress_tile(
  struct _openslide_czi_tile_descriptor  * tile_desc,
  uint8_t                                * data,
  int32_t                                  data_size,
  int32_t                                * uncompressed_data_size,
  GError                                ** err
)
{

  uint8_t * uncompressed_data = NULL;

  if (!tile_desc){
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Invalid tile descriptor to uncompress data" );
    (*uncompressed_data_size) = 0;
    return NULL;
  }

  if (!data){
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Invalid data to uncompress" );
    (*uncompressed_data_size) = 0;
    return NULL;
  }

  // Uncompress data using openslide implemented methods
  const struct _openslide_czi_uncompressor * uncompressor = NULL;

  switch(tile_desc->compression){
    case JPEG:
      uncompressor = (&_openslide_uncompressor_jpeg);
      break;

    case JPEGXR:
#ifdef HAVE_LIBJXR
      uncompressor = (&_openslide_uncompressor_jxr);
      break;
#endif //HAVE_LIBJXR

    case LZW:
    case CAMERA_SPEC:
    case SYSTEM_SPEC:
      // Compression method is not implemented
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Compression method %s is not yet supported",
                   czi_compression_t_string(tile_desc->compression) );
      (*uncompressed_data_size) = 0;
      return NULL;

    case UNCOMPRESSED:
      // Data are uncompressed
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Data are uncompressed" );
      (*uncompressed_data_size) = 0;
      return NULL;

    default:
    case CMP_UNKNOWN:
      // Data are uncompressed
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Compression method is unknown" );
      (*uncompressed_data_size) = 0;
      return NULL;
  }

  uncompressed_data = _openslide_czi_uncompress(
                            uncompressor,
                            data, data_size,
                            tile_desc->size_x / tile_desc->subsampling_x,
                            tile_desc->size_y / tile_desc->subsampling_y,
                            tile_desc->pixel_type,
                            uncompressed_data_size,
                            err);

  return uncompressed_data;
}


uint8_t * _openslide_czi_load_tile(
  _openslide_czi  * czi,
  int32_t           level,
  int64_t           uid,
  int32_t         * buffer_size,
  GError         ** err
)
{
  struct _czi_level * s_level = g_ptr_array_index( czi->levels, level );
  if( !s_level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return NULL;
  }
  struct _czi_tile * tile = g_hash_table_lookup( s_level->tiles, &uid );
  if( !tile ){
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find tile %ld", uid );
    return NULL;
  }

  g_assert( tile->source );
  if( !tile->source->stream ) {
    tile->source->stream = _openslide_fopen( tile->source->filename, "rb", err );
    if( !tile->source->stream ) return NULL;
  }

  FILE * stream = tile->source->stream;

  // Seek to the beginning of SubBlockSegment for the tile
  TRY_FSEEKO( stream, tile->tile_offset, SEEK_SET, err, "Failed to load tile" );

  // Read SubBlockSegment header
  struct _czi_segment_header * header = (struct _czi_segment_header*) g_slice_alloc0( sizeof(struct _czi_segment_header) );
  if(!czi_read_next_segment_header(tile->source, header, err))
  {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to read tile %ld header", uid );

    g_slice_free( struct _czi_segment_header, header );
    return NULL;

  }
  g_slice_free( struct _czi_segment_header, header );

  TRY_READ_ITEMS( &(tile->metadata_size),       1, 4, stream, err, "Failed to read metadata_size for tile" );      // MetaDataSize
  TRY_READ_ITEMS( &(tile->attachment_size),     1, 4, stream, err, "Failed to read attachment_size for tile" );    // AttachmentSize
  TRY_READ_ITEMS( &(tile->data_size),           1, 8, stream, err, "Failed to read data_size for tile" );          // DataSize

  // Read DimensionCount integer
  int32_t dimension_count;
  int64_t position = ftello(stream);
  TRY_FSEEKO( stream, position + 28, SEEK_SET, err, "Failed to get dimension count for tile" );
  TRY_READ_ITEMS( &dimension_count,             1, 4, stream, err, "Failed to read dimension_count for tile" );    // DimensionCount

  // Seek to data offset
  // Data offset = MAX(256, DimensionEntries offset + DimensionCount * 20) + Metada size
  // Data offset = MAX(256, 32 + 16 + DimensionCount * 20) + Metada size
  // DimensionEntries offset = 32 + 16
  // Data offset = DimensionEntries offset + MAX(256 - 32 - 16, DimensionCount * 20) + Metada size
  // Data offset = DimensionEntries offset + MAX(208, DimensionCount * 20)
  position = ftello(stream) + MAX(208, (20 * dimension_count)) + tile->metadata_size;
  TRY_FSEEKO( stream, position, SEEK_SET, err, "Failed to seek to start data positioni of tile" );

  //g_debug("_openslide_czi_load_tile:: tile:%ld, data_size:%d", uid, tile->data_size);
  
  // Allocate a buffer to read tile data from file without decompression
  tile->data_buf = g_slice_alloc0( tile->data_size );
  if( !tile->data_buf ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %d bytes", tile->data_size );
    return NULL;
  }
  
  if( !read_items( tile->data_buf, 1, tile->data_size, stream, err ) ) {
    g_prefix_error( err, "Failed to load tile: " );
    g_slice_free1( tile->data_size, tile->data_buf );
    tile->data_buf = NULL;
    return NULL;
  }

#ifdef CZI_WRITE_TILE_DATA
  char * filename = g_strdup_printf( "tile_%d_%ld", level, tile->uid);
  FILE * outstream = _openslide_fopen( filename, "w+", err );
  if (outstream) {
    uint64_t len;
    len = fwrite( tile->data_buf, 1, tile->data_size, outstream );
    if( len != (uint64_t)tile->data_size ) {
      g_debug( "Unable to write tile %ld data to file %s", tile->uid, filename );
    }
  }
#endif

  if( buffer_size )  (*buffer_size) = tile->data_size;

  return tile->data_buf;
}

bool _openslide_czi_destroy_tile(
  _openslide_czi  * czi,
  int32_t           level,
  int64_t           uid,
  GError         ** err
)
{
  struct _czi_level * s_level = g_ptr_array_index( czi->levels, level );
  if( !s_level ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level %d", level );
    return false;
  }
  struct _czi_tile * tile = g_hash_table_lookup( s_level->tiles, &uid );
  if( !tile ){
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find tile %ld", uid );
    return false;
  }
  if( tile->data_buf ) {
    g_slice_free1( tile->data_size, tile->data_buf );
    tile->data_buf = NULL;
  }

  return true;
}

void _openslide_czi_free_list_tiles( GList * list )
{
  GList * current = list;
  while( current )
  {
    czi_free_tile_descriptor( current->data );
    current = g_list_next( current );
  }
  current = g_list_previous( list );
  while( current )
  {
    czi_free_tile_descriptor( current->data );
    current = g_list_previous( current );
  }
  g_list_free( list );
}

//--- metadata ---------------------------------------------------------------
int32_t _openslide_czi_get_metadata_count( _openslide_czi * czi )
{
  return czi->metadata->len;
}

char * _openslide_czi_load_metadata(
  _openslide_czi  * czi,
  int32_t           index,
  int32_t         * buffer_size,
  GError         ** err
)
{
  struct _czi_metadata * metadata = g_ptr_array_index( czi->metadata, index );
  if( !metadata ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to access metadata block %d", index );
    return NULL;
  }

  if( !metadata->source->stream ) {
    metadata->source->stream = _openslide_fopen( metadata->source->filename , "rb", err );
    if( !metadata->source->stream) return NULL;
  }
  FILE * stream = metadata->source->stream;
  TRY_FSEEKO( stream, metadata->offset, SEEK_SET, err, "Failed to load metadata" );
  metadata->xml_buf = g_slice_alloc0( metadata->xml_size + 1 );
  if( !metadata->xml_buf ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to allocate %d bytes", metadata->xml_size );
    return NULL;
  }
  if( !read_items( metadata->xml_buf, 1, metadata->xml_size, stream, err ) ) {
    g_prefix_error( err, "Failed to load metadata %d", index );
    g_slice_free1( metadata->xml_size, metadata->xml_buf );
    metadata->xml_buf = NULL;
    return NULL;
  }

  metadata->xml_buf[metadata->xml_size] = '\0';
  if( buffer_size ) *buffer_size = metadata->xml_size+1;
  return metadata->xml_buf;
}

bool _openslide_czi_destroy_metadata(
  _openslide_czi  * czi,
  int32_t           index,
  GError         ** err
)
{
  struct _czi_metadata * metadata = g_ptr_array_index( czi->metadata, index );
  if( !metadata ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find metadata %d", index );
    return false;
  }
  if( metadata->xml_buf ) {
    g_slice_free1( metadata->xml_size+1, metadata->xml_buf );
    metadata->xml_buf = NULL;
  }

  return true;
}

//--- attachments ------------------------------------------------------------
_openslide_czi * _openslide_czi_decode_label(
  _openslide_czi  * czi  G_GNUC_UNUSED,
  GError         ** err  G_GNUC_UNUSED
)
{
  /*TODO*/
  return NULL;
}

_openslide_czi * _openslide_czi_decode_prescan(
  _openslide_czi  * czi  G_GNUC_UNUSED,
  GError         ** err  G_GNUC_UNUSED
)
{
  /*TODO*/
  return NULL;
}

_openslide_czi * _openslide_czi_decode_slide_preview(
  _openslide_czi  * czi  G_GNUC_UNUSED,
  GError         ** err  G_GNUC_UNUSED
)
{
  /*TODO*/
  return NULL;
}

//--- free -------------------------------------------------------------------
void _openslide_czi_free( struct _czi * ptr )
{
  czi_free( ptr );
}

//--- caracteristics ---------------------------------------------------------
bool _openslide_czi_is_multi_view( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_view;
}

bool _openslide_czi_is_multi_phase( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_phase;
}

bool _openslide_czi_is_multi_block( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_block;
}

bool _openslide_czi_is_multi_illumination( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_illumination;
}

bool _openslide_czi_is_multi_rotation( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_rotation;
}

bool _openslide_czi_is_multi_time( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_time;
}

bool _openslide_czi_is_multi_zslice( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_zslice;
}

bool _openslide_czi_is_multi_channel( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->is_multi_channel;
}

bool _openslide_czi_has_data_uncompressed( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->has_data_uncompressed;
}

bool _openslide_czi_has_data_jpg( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->has_data_jpg;
}

bool _openslide_czi_has_data_jpgxr( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->has_data_jpgxr;
}

bool _openslide_czi_has_data_lzw( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->has_data_lzw;
}

bool _openslide_czi_has_data_cameraspec( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->has_data_cameraspec;
}

bool _openslide_czi_has_data_systemspec( _openslide_czi * czi )
{
  g_assert( czi );
  return czi->has_data_systemspec;
}

// key:DRIVER-TOP
//////////////////////////////////////////////////////////////////////////////
///                                                                        ///
///                        Z E I S S   D R I V E R                         ///
///                                                                        ///
//////////////////////////////////////////////////////////////////////////////

// key:DRIVER-API-DECL
//============================================================================
//
//              ZEISS VENDOR - OPENSLIDE API : DECLARATIONS
//
//============================================================================

//============================================================================
//   API METHODS
//============================================================================

static bool zeiss_open(
  openslide_t                 * osr,
  const char                  * filename,
  struct _openslide_tifflike  * tl,
  struct _openslide_hash      * quickhash1,
  GError                     ** err
);

static bool zeiss_detect(
  const char                  * filename,
  struct _openslide_tifflike  * tl,
  GError                     ** err
);

static void zeiss_destroy(
  openslide_t * osr
);

static bool zeiss_paint_region(
  openslide_t               * osr,
  cairo_t                   * cr,
  int64_t                     x,
  int64_t                     y,
  struct _openslide_level   * level,
  int32_t                     w,
  int32_t                     h,
  GError                   ** err
);

static bool zeiss_tileread(
  openslide_t               * osr,
  cairo_t                   * cr,
  struct _openslide_level   * level,
  int64_t                     tile_unique_id,
  void                      * tile,
  void                      * arg,
  GError                   ** err
);

//============================================================================
//   STRUCTURE
//============================================================================
const struct _openslide_format _openslide_format_zeiss = {
  .name   = "zeiss",
  .vendor = "zeiss",
  .detect = zeiss_detect,
  .open   = zeiss_open,
};

static const struct _openslide_ops _openslide_ops_zeiss = {
  .paint_region = zeiss_paint_region,
  .destroy      = zeiss_destroy,
};

// key:DRIVER-PRI-DECL
//============================================================================
//
//              ZEISS VENDOR - PRIVATE METHODS : DECLARATIONS
//
//============================================================================
#ifdef CZI_DEBUG
static void zeiss_debug_display_tile_counts( openslide_t * osr, GHashTable * tilesinfo, bool details );
#endif
static bool zeiss_check( _openslide_czi * czi, GError ** err );
static bool zeiss_set_properties( openslide_t * osr, _openslide_czi * czi, GError ** err );
static bool zeiss_set_levels( openslide_t * osr, _openslide_czi * czi, GError ** err );
static bool zeiss_set_rois( openslide_t * osr, _openslide_czi * czi, GError ** err ) G_GNUC_UNUSED;
static bool zeiss_set_grids( openslide_t * osr, _openslide_czi * czi, GError ** err );

//============================================================================
//   ZEISS PROPERTIES
//============================================================================

#define ZEISS_IMAGESIZE_X      "zeiss.information.image.size-x"
#define ZEISS_IMAGESIZE_Y      "zeiss.information.image.size-y"
#define ZEISS_IMAGESIZE_C      "zeiss.information.image.size-c"
#define ZEISS_IMAGESIZE_Z      "zeiss.information.image.size-z"
#define ZEISS_IMAGESIZE_T      "zeiss.information.image.size-t"
#define ZEISS_IMAGESIZE_H      "zeiss.information.image.size-h"
#define ZEISS_IMAGESIZE_R      "zeiss.information.image.size-r"
#define ZEISS_IMAGESIZE_S      "zeiss.information.image.size-s"
#define ZEISS_IMAGESIZE_I      "zeiss.information.image.size-i"
#define ZEISS_IMAGESIZE_M      "zeiss.information.image.size-m"
#define ZEISS_IMAGESIZE_B      "zeiss.information.image.size-b"
#define ZEISS_IMAGESIZE_V      "zeiss.information.image.size-v"
#define ZEISS_ACQ_DATE         "zeiss.information.image.acquisition-date-and-time"
#define ZEISS_ACQ_DURATION     "zeiss.information.image.acquisition-duration"
#define ZEISS_PIXEL_TYPE       "zeiss.information.image.pixel-type"

#define ZEISS_BIT_COUNT        "zeiss.information.image.component-bit-count"
#define ZEISS_CH_COUNT         "zeiss.information.image.dimensions.channel-count"
#define ZEISS_CH_NAME          "zeiss.information.image.dimensions.channel[%d].name"
#define ZEISS_CH_PIXEL_TYPE    "zeiss.information.image.dimensions.channel[%d].pixel_type"
#define ZEISS_CH_BIT_COUNT     "zeiss.information.image.dimensions.channel[%d].component-bit-count"
#define ZEISS_CH_ACQMODE       "zeiss.information.image.dimensions.channel[%d].acquisition-mode"
#define ZEISS_CH_ILTYPE        "zeiss.information.image.dimensions.channel[%d].illumination-type"
#define ZEISS_CH_CONTRAST      "zeiss.information.image.dimensions.channel[%d].constrast-method"
#define ZEISS_CH_FLUOR         "zeiss.information.image.dimensions.channel[%d].fluor"
#define ZEISS_CH_COLOR         "zeiss.information.image.dimensions.channel[%d].color"
#define ZEISS_CH_EXPTIME       "zeiss.information.image.dimensions.channel[%d].exposure-time"
#define ZEISS_CH_THCK          "zeiss.information.image.dimensions.channel[%d].section-thickness"

#define ZEISS_COMP_UNKNOWN     "zeiss.information.image.compressions.has-unknown-tiles"
#define ZEISS_COMP_UNCOMP      "zeiss.information.image.compressions.has-uncompressed-tiles"
#define ZEISS_COMP_JPEG        "zeiss.information.image.compressions.has-jpeg-tiles"
#define ZEISS_COMP_LZW         "zeiss.information.image.compressions.has-lzw-tiles"
#define ZEISS_COMP_JPEGXR      "zeiss.information.image.compressions.has-jpegxr-tiles"
#define ZEISS_COMP_CAMSPEC     "zeiss.information.image.compressions.has-camera-specific-tiles"
#define ZEISS_COMP_SYSSPEC     "zeiss.information.image.compressions.has-system-specific-tiles"

#define ZEISS_OBJ_COUNT        "zeiss.information.instrument.objective-count"
#define ZEISS_OBJ_NAME         "zeiss.information.instrument.objective[%d].objective-name"
#define ZEISS_OBJ_LENSNA       "zeiss.information.instrument.objective[%d].lens-na"
#define ZEISS_OBJ_MAGN         "zeiss.information.instrument.objective[%d].nominal-magnification"
#define ZEISS_OBJ_DIST         "zeiss.information.instrument.objective[%d].working-distance"
#define ZEISS_OBJ_GEOM         "zeiss.information.instrument.objective[%d].pupil-geometry"
#define ZEISS_OBJ_IMMERSION    "zeiss.information.instrument.objective[%d].immersion"

#define ZEISS_SC_X             "zeiss.scaling.distance-x.value"
#define ZEISS_SC_Y             "zeiss.scaling.distance-y.value"

#define ZEISS_ACQBLOCK_COUNT     "zeiss.experiment.acquisition-block-count"
#define ZEISS_OVERLAP            "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.overlap"
#define ZEISS_COVERING_MODE      "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region-covering-mode"
#define ZEISS_TILEREGION_COUNT   "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region-count"
#define ZEISS_TILEREGION_CENTER  "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].center-position"
#define ZEISS_TILEREGION_CONTOUR "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].contour-size"
#define ZEISS_TILEREGION_COLUMNS "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].columns"
#define ZEISS_TILEREGION_ROWS    "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].rows"
#define ZEISS_TILEREGION_Z       "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].z"
#define ZEISS_TILEREGION_ACQ     "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].is-used-for-acquisition"
#define ZEISS_TILEREGION_PROTECT "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].is-protected"
#define ZEISS_TILEREGION_CTYPE   "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.tile-region[%d].contour-type"


// Used for openslide properties
#define ZEISS_VOXELSIZE_X      ZEISS_SC_X
#define ZEISS_VOXELSIZE_Y      ZEISS_SC_Y
#define ZEISS_MAGNIFICATION    "zeiss.information.instrument.objective[0].nominal-magnification"
#define ZEISS_BG_COLOR         "zeiss.information.image.dimensions.channel[0].color"

#define ZEISS_SET_PROP( osr, context, property_format, path_format, ... )             \
  {                                                                                   \
    char *property, *path;                                                            \
    property = g_strdup_printf( property_format, ##__VA_ARGS__ );                     \
    path = g_strdup_printf( path_format, ##__VA_ARGS__ );                             \
    _openslide_xml_set_prop_from_xpath( osr, context, property, path );               \
    g_free( property );                                                               \
    g_free( path );                                                                   \
  }                                                                                   \
  (void)0

#define ZEISS_GET_FORMATTED_PROP( osr, property_format, property_value, ... )         \
  {                                                                                   \
    char *property;                                                                   \
    property = g_strdup_printf( property_format, ##__VA_ARGS__ );                     \
    property_value = g_hash_table_lookup( osr->properties, property );                \
    g_free( property );                                                               \
  }                                                                                   \
  (void)0

#define ZEISS_GET_PROP( osr, property, property_value )                               \
  ZEISS_GET_FORMATTED_PROP( osr, property"%s", property_value, "" );                  \
  (void)0


//============================================================================
//   TEMPORARY HELP: FORMAT-SPECIFIC KEYS
//============================================================================

#if 0
struct _openslide {
  const struct _openslide_ops     * ops;
        struct _openslide_level  ** levels;
        void                      * data;
        int32_t                     level_count;
        GHashTable                * associated_images;
        const char               ** associated_image_names;
        GHashTable                * properties;
  const char                     ** property_names;
        struct _openslide_cache   * cache;
        gpointer                    error;
};

struct _openslide_ops {
  bool (*paint_region)(
           openslide_t              * osr,
           cairo_t                  * cr,
           int64_t                    x,
           int64_t                    y,
           struct _openslide_level  * level,
           int32_t                    w,
           int32_t                    h,
           GError                  ** err);
  void (*destroy)( openslide_t *osr );
};
#endif

// key:DRIVER-PRI-DEF
//============================================================================
//
//              ZEISS VENDOR - PRIVATE METHODS : DEFINITIONS
//
//============================================================================
#ifdef CZI_DEBUG
void zeiss_debug_display_tile_counts(openslide_t * osr,
                                     GHashTable  * tile_infos,
                                     bool details){
  // Display information about tiles access
  GHashTableIter iter;
  gpointer key, value;
  
  struct _czi                           * czi = (struct _czi *)osr->data;
  struct _czi_level                     * level;
  struct _czi_tile                      * tile;
  struct _openslide_czi_tile_descriptor * tile_desc;
  int64_t                                 tiles_sum = 0, tiles_count = 0;
        
  g_hash_table_iter_init(&iter, tile_infos);
  while(g_hash_table_iter_next(&iter, &key, &value)) {
    tiles_sum += *(int64_t *)value;
    tiles_count++;

    if (details) {
      tile = NULL;
      // Try to find the tile in levels
      for (uint32_t l = 0; l < czi->levels->len; ++l) {
        level = (struct _czi_level *)g_ptr_array_index(czi->levels, l);
        tile = (struct _czi_tile *)g_hash_table_lookup(level->tiles, key);
        
        if (tile) break;
      }

      if (tile) {
        tile_desc = czi_new_tile_descriptor(tile, 0); 
        g_debug("tile %ld at %d %d accessed %ld times.", 
                *(int64_t *)key, 
                tile_desc->start_x,
                tile_desc->start_y,
                *(int64_t *)value);
        czi_free_tile_descriptor(tile_desc);
      }
      else {
        g_debug("tile %ld (not found in tiles) %ld times.", 
                *(int64_t *)key, 
                *(int64_t *)value);
      }
    }
  }
  
  if (tiles_count > 0)
    g_debug("%ld tiles, %ld accessed, average of %f per tile.", 
            tiles_count, 
            tiles_sum,
            (float)tiles_sum / (float)tiles_count);
}
#endif

bool zeiss_check(
  _openslide_czi  * czi,
  GError          ** err
)
{
  // Look for unsupported images
  if( _openslide_czi_is_multi_view( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple views not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_phase( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple phases not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_block( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple blocks not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_illumination( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple illuminations not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_rotation( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple rotations not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_time( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple time points not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_zslice( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Z stacks not supported" );
    return false;
  }
  if( _openslide_czi_is_multi_channel( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Multiple channels not supported" );
    return false;
  }

#ifndef HAVE_LIBJXR
  if( _openslide_czi_has_data_jpgxr( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "JPEGXR compression not supported" );
    return false;
  }
#endif

  if( _openslide_czi_has_data_lzw( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "LZW compression not supported" );
    return false;
  }
  if( _openslide_czi_has_data_cameraspec( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Camera specific compression not supported" );
    return false;
  }
  if( _openslide_czi_has_data_systemspec( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "System specific compression not supported" );
    return false;
  }

  return true;
}

bool zeiss_set_properties(
  openslide_t     * osr,
  _openslide_czi  * czi,
  GError         ** err
)
{
  //--- decode xml block -----------------------------------------------------

  int32_t meta_count = _openslide_czi_get_metadata_count( czi );
  if( meta_count <= 0 ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "No metadata block to load" );
    return false;
  }

  // load metadata buffer n0
  // (we suppose all files contain the same xml block)
  char * xml_buffer = NULL;
  int32_t   xml_size;
  xml_buffer = _openslide_czi_load_metadata( czi, 0, &xml_size, err );
  if( !xml_buffer )
    return false;

#ifdef CZI_WRITE_XML
  FILE * streamout = _openslide_fopen( "/tmp/zeiss.xml", "wb", 0 );
  fwrite( xml_buffer, xml_size, 1, streamout );
  fclose( streamout );
  g_debug("XML data written to /tmp/zeiss.xml");
#endif

  // convert xml
  xmlDoc * xml_doc = NULL;
  xml_doc = _openslide_xml_parse( (char*) xml_buffer, err );
  if( !xml_doc ) return false;
  if( !_openslide_czi_destroy_metadata( czi, 0, err ) ) return false;

  // get path context
  xmlXPathContext * xml_path_context = _openslide_xml_xpath_create( xml_doc );
  if( !xml_path_context ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "XML conversion to XPath context failed." );
    return false;
  }

  //--- set vendor properties ------------------------------------------------
  xmlXPathObject * xml_path_object = NULL;

  // Information / Image
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_X,
    "/ImageDocument/Metadata/Information/Image/SizeX" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_Y,
    "/ImageDocument/Metadata/Information/Image/SizeY" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_C,
    "/ImageDocument/Metadata/Information/Image/SizeC" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_Z,
    "/ImageDocument/Metadata/Information/Image/SizeZ" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_T,
    "/ImageDocument/Metadata/Information/Image/SizeT" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_H,
    "/ImageDocument/Metadata/Information/Image/SizeH" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_R,
    "/ImageDocument/Metadata/Information/Image/SizeR" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_S,
    "/ImageDocument/Metadata/Information/Image/SizeS" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_I,
    "/ImageDocument/Metadata/Information/Image/SizeI" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_M,
    "/ImageDocument/Metadata/Information/Image/SizeM" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_B,
    "/ImageDocument/Metadata/Information/Image/SizeB" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_IMAGESIZE_V,
    "/ImageDocument/Metadata/Information/Image/SizeV" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_ACQ_DATE,
    "/ImageDocument/Metadata/Information/Image/AcquisitionDateAndTime" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_ACQ_DURATION,
    "/ImageDocument/Metadata/Information/Image/AcquisitionDuration" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_PIXEL_TYPE,
    "/ImageDocument/Metadata/Information/Image/PixelType" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_BIT_COUNT,
    "/ImageDocument/Metadata/Information/Image/ComponentBitCount" );
  
  // Information / Image / Compression
  g_hash_table_insert( osr->properties,
                       g_strdup( ZEISS_COMP_UNCOMP ),
                       g_strdup(
                           czi_boolean_t_string(
                                _openslide_czi_has_data_uncompressed( czi )
                       )));
  g_hash_table_insert( osr->properties,
                       g_strdup( ZEISS_COMP_JPEG ),
                       g_strdup(
                           czi_boolean_t_string(
                                _openslide_czi_has_data_jpg( czi )
                       )));
  g_hash_table_insert( osr->properties,
                       g_strdup( ZEISS_COMP_LZW ),
                       g_strdup(
                           czi_boolean_t_string(
                                _openslide_czi_has_data_lzw( czi )
                       )));
  g_hash_table_insert( osr->properties,
                       g_strdup( ZEISS_COMP_JPEGXR ),
                       g_strdup(
                           czi_boolean_t_string(
                                _openslide_czi_has_data_jpgxr( czi )
                       )));
  g_hash_table_insert( osr->properties,
                       g_strdup( ZEISS_COMP_CAMSPEC ),
                       g_strdup(
                           czi_boolean_t_string(
                                _openslide_czi_has_data_cameraspec( czi )
                       )));
  g_hash_table_insert( osr->properties,
                       g_strdup( ZEISS_COMP_SYSSPEC ),
                       g_strdup(
                           czi_boolean_t_string(
                                _openslide_czi_has_data_systemspec( czi )
                       )));

  // Information / Image / Dimensions
  int32_t channel_count = 0;
  xml_path_object = xmlXPathEvalExpression(BAD_CAST
    "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel",
    xml_path_context );
  if( xml_path_object && xml_path_object->nodesetval )
    channel_count = xml_path_object->nodesetval->nodeNr;
  xmlXPathFreeObject( xml_path_object );
  g_hash_table_insert( osr->properties, g_strdup(ZEISS_CH_COUNT),
    _openslide_format_double(channel_count) );
  for( int32_t i=0; i<channel_count; ++i )
  {

    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_NAME,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/@Name", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_PIXEL_TYPE,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/PixelType", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_BIT_COUNT,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/ComponentBitcount", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_ACQMODE,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/AcquisitionMode", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_ILTYPE,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/IlluminationType", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_CONTRAST,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/ContrastMethod", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_FLUOR,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/Fluor", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_COLOR,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/Color", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_EXPTIME,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/ExposureTime", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_THCK,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d+1]"
      "/SectionThickness", i );
  }

  // Information / Instrument / Objectives
  int32_t obj_count = 0;
  xml_path_object = xmlXPathEvalExpression(BAD_CAST
    "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective",
    xml_path_context );
  if( xml_path_object && xml_path_object->nodesetval )
    obj_count = xml_path_object->nodesetval->nodeNr;
  xmlXPathFreeObject( xml_path_object );
  g_hash_table_insert( osr->properties, g_strdup(ZEISS_OBJ_COUNT), _openslide_format_double(obj_count) );
  for( int32_t i=0; i<obj_count; ++i )
  {
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_NAME,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d+1]"
      "/ObjectiveName", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_LENSNA,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d+1]"
      "/LensNA", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_MAGN,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d+1]"
      "/NominalMagnification", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_DIST,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d+1]"
      "/WorkingDistance", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_GEOM,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d+1]"
      "/PupilGeometry", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_IMMERSION,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d+1]"
      "/Immersion", i );
  }

  // Scaling
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_SC_X,
    "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='X']/Value" );
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context, ZEISS_SC_Y,
    "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='Y']/Value" );

  // Experiment / AcquisitionBlocks
  int32_t block_count = 0;
  xml_path_object = xmlXPathEvalExpression(BAD_CAST
    "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock",
    xml_path_context );
  if( xml_path_object && xml_path_object->nodesetval )
    block_count = xml_path_object->nodesetval->nodeNr;
  xmlXPathFreeObject( xml_path_object );
  g_hash_table_insert( osr->properties,
    g_strdup(ZEISS_ACQBLOCK_COUNT),
    _openslide_format_double(block_count) );
  for( int32_t i=0; i<block_count; ++i )
  {
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OVERLAP,
      "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
      "/SubDimensionSetups/RegionsSetup/SampleHolder/Overlap", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_COVERING_MODE,
      "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
      "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegionCoveringMode", i );

    int32_t region_count = 0;
    char * path = g_strdup_printf(
      "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
      "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion", i );
    xml_path_object = xmlXPathEvalExpression(BAD_CAST path, xml_path_context );
    if( xml_path_object && xml_path_object->nodesetval )
      region_count = xml_path_object->nodesetval->nodeNr;
    xmlXPathFreeObject( xml_path_object );
    g_free( path );
    g_hash_table_insert( osr->properties,
      g_strdup_printf( ZEISS_TILEREGION_COUNT, i ),
      _openslide_format_double(region_count) );

    for( int32_t j=0; j<region_count; ++j )
    {
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_CENTER,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/CenterPosition", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_CONTOUR,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/ContourSize", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_COLUMNS,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/Columns", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_ROWS,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/Rows", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_Z,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/Z", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_ACQ,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/IsUsedForAcquisition", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_PROTECT,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/IsProtected", i, j );
      ZEISS_SET_PROP( osr, xml_path_context, ZEISS_TILEREGION_CTYPE,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d+1]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d+1]"
        "/Contour/@Type", i, j );
    }
  }

  xmlFreeDoc( xml_doc );
  xmlXPathFreeContext( xml_path_context );

  //--- set openslide properties ---------------------------------------------
  //--- parse voxel sizes ----------------------------------------------------
  double mpp;
  const char *mppc;
  
  mpp = 0;
  mppc = (const char*)g_hash_table_lookup( osr->properties, ZEISS_VOXELSIZE_X );
  if (mppc) {
    mpp = _openslide_parse_double(mppc);
    mpp *= 1e6;
  }
  if (mpp == 0){
      g_debug("Size of pixels along X axis is unknown. Uses 1 as a default value.");
      mpp = 1;
  }
  g_hash_table_insert( osr->properties, g_strdup( OPENSLIDE_PROPERTY_NAME_MPP_X ), _openslide_format_double(mpp) );

  mpp = 0;
  mppc = (const char*)g_hash_table_lookup( osr->properties, ZEISS_VOXELSIZE_Y );
  if (mppc) {
    mpp = _openslide_parse_double(mppc);
    mpp *= 1e6;
  }
  if (mpp == 0){
      g_debug("Size of pixels along Y axis is unknown. Uses 1 as a default value.");
      mpp = 1;
  }
  g_hash_table_insert( osr->properties, g_strdup( OPENSLIDE_PROPERTY_NAME_MPP_Y ), _openslide_format_double(mpp) );

  //--- parse other properties -----------------------------------------------
  _openslide_duplicate_int_prop( osr, ZEISS_MAGNIFICATION, OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER );

  const char * bg = (const char *) g_hash_table_lookup( osr->properties, ZEISS_BG_COLOR );
  if( bg )
  {
    uint8_t orig = 1;
    if( strlen(bg) == 9 ) orig = 3;
    char * red = g_strndup( bg+orig, 2 );
    char * green = g_strndup( bg+orig+2, 2 );
    char * blue = g_strndup( bg+orig+4, 2 );
    _openslide_set_background_color_prop( osr,
      strtol(red,NULL,16), strtol(green,NULL,16), strtol(blue,NULL,16) );
    g_free( red );
    g_free( green );
    g_free( blue );
  }

  return true;
}

bool zeiss_set_levels(
  openslide_t     * osr,
  _openslide_czi  * czi,
  GError         ** err
)
{
  int32_t subsampling, th, tw;
  char *w, *h;
  int32_t level_count = _openslide_czi_get_level_count( czi );
  GPtrArray * array_levels = g_ptr_array_sized_new( level_count );
  struct _openslide_level * level;

  for( int32_t i=0; i<level_count; ++i )
  {
    subsampling = _openslide_czi_get_level_subsampling( czi, i, err );
    if( subsampling == 0 ) {
      osr->level_count = i;
      zeiss_destroy( osr );
      return false;
    }
    level = g_slice_alloc0( sizeof( struct _openslide_level ) );
    level->downsample = (double) subsampling;
    w = g_hash_table_lookup( osr->properties, ZEISS_IMAGESIZE_X );
    level->w = (int64_t)( _openslide_parse_double( w ) / level->downsample );
    h = g_hash_table_lookup( osr->properties, ZEISS_IMAGESIZE_Y );
    level->h = (int64_t)( _openslide_parse_double( h ) / level->downsample );
    if( !_openslide_czi_get_level_tile_size( czi, i, &tw, &th, err ) ) {
      g_slice_free( struct _openslide_level, level );
      osr->level_count = i;
      zeiss_destroy( osr );
      return false;
    }
    level->tile_w = (int64_t) tw;
    level->tile_h = (int64_t) th;
    g_ptr_array_add( array_levels, level );
  }

  osr->level_count = level_count;
  osr->levels = ( struct _openslide_level **) g_ptr_array_free( array_levels, false );
  return true;
}


bool zeiss_set_rois(
  openslide_t     * osr,
  _openslide_czi  * czi,
  GError         ** err
)
{
  int32_t block_count = 0;
  int32_t roi_count = 0;
  double overlap = 0;
  enum czi_roi_covering_mode_t covering_mode;
  char * value;
  GPtrArray * array_rois;
  struct _czi_roi * roi;
  char ** l;

  // Get block count
  ZEISS_GET_PROP( osr, ZEISS_ACQBLOCK_COUNT, value );
  block_count = (int32_t)_openslide_parse_double( (const char *)value );

  // TODO: Manage multiple block rois
  // As it is currently not possible to select a block, a possibility may be to
  // warn about multiple blocks but only use the first one
  if (block_count > 1) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "Unable to manage multiple acquisition blocks" );
    return false;
  }

  // Iterate trough acquisition blocks to get roi information
  for ( int32_t b = 0; b < block_count; ++b )
  {
    // Rois count;
    ZEISS_GET_FORMATTED_PROP( osr, ZEISS_TILEREGION_COUNT, value, b );
    roi_count = (int32_t)_openslide_parse_double( (const char *)value );

    // Tiles information
    ZEISS_GET_FORMATTED_PROP( osr, ZEISS_OVERLAP, value, b );
    overlap = (double)_openslide_parse_double( (const char *)value );

    ZEISS_GET_FORMATTED_PROP( osr, ZEISS_COVERING_MODE, value, b );
    if (!strcasecmp(value, czi_cov_aligned_to_global_grid)) {
      covering_mode = ALIGNED_TO_GLOBAL_GRID;
    }
    else if (!strcasecmp(value, czi_cov_aligned_to_local_tile_region)) {
      covering_mode = ALIGNED_TO_LOCAL_TILE_REGION;
    }
    else {
      covering_mode = COV_UNKNOWN;
    }

    // Allocate array of rois
    array_rois = g_ptr_array_sized_new( roi_count );

    for ( int32_t r = 0; r < roi_count; ++r)
    {
      roi = czi_new_roi(czi, err);

      roi->overlap = overlap;
      roi->covering_mode = covering_mode;

      ZEISS_GET_FORMATTED_PROP( osr, ZEISS_TILEREGION_CTYPE, value, b, r );
      if (!strcasecmp(value, ellipse)) {
        roi->shape = ELLIPSE;
      }
      else if (!strcasecmp(value, rectangle)) {
        roi->shape = RECTANGLE;
      }
      else if (!strcasecmp(value, polygon)) {
        roi->shape = POLYGON;
      }
      else {
        roi->shape = SHP_UNKNOWN;
      }

      ZEISS_GET_FORMATTED_PROP( osr, ZEISS_TILEREGION_COLUMNS, value, b, r );
      roi->columns = (int32_t)_openslide_parse_double( (const char *)value );

      ZEISS_GET_FORMATTED_PROP( osr, ZEISS_TILEREGION_ROWS, value, b, r );
      roi->rows = (int32_t)_openslide_parse_double( (const char *)value );

      ZEISS_GET_FORMATTED_PROP( osr, ZEISS_TILEREGION_CONTOUR, value, b, r );
      l = g_strsplit( (const char *)value, ",", -1);

      if (l[0] != NULL) {
        roi->w = _openslide_parse_double( (const char *)l[0] );
      }
      else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Unable to get width for ROI" );
        return false;
      }

      if (l[1] != NULL) {
        roi->h = _openslide_parse_double( (const char *)l[1] );
      }
      else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Unable to get height for ROI" );
        return false;
      }

      ZEISS_GET_FORMATTED_PROP( osr, ZEISS_TILEREGION_CENTER, value, b, r );
      l = g_strsplit( (const char *)value, ",", -1);

      if (l[0] != NULL) {
        roi->x = _openslide_parse_double( (const char *)l[0] ) - (roi->w / 2);
      }
      else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Unable to get X coordinate for ROI" );
        return false;
      }

      if (l[1] != NULL) {
        roi->y = _openslide_parse_double( (const char *)l[1] ) - (roi->h / 2);
      }
      else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "Unable to get Y coordinate for ROI" );
        return false;
      }

      // czi_display_roi(roi, 0);

      g_ptr_array_add( array_rois, roi);
    }

    czi->rois = array_rois;
  }

  return true;
}

bool zeiss_set_grids( openslide_t     * osr,
                      _openslide_czi  * czi,
                      GError         ** err)
{

  struct _openslide_level * level;
  struct _openslide_grid * grid;
  int32_t level_count = osr->level_count;
  int32_t * downsample;
  int32_t offset_x, offset_y;

  //g_debug("zeiss_set_grids::level_count: %d", level_count);

  for (int32_t l = 0; l < level_count; ++l) {

    level = osr->levels[l];
    if (!level)
      g_debug("zeiss_set_grids::level not found: %d", l);

    // Get level offset
    _openslide_czi_get_level_tile_offset( czi, l, &offset_x, &offset_y, err );

    // Instanciate new grid for current level
    //g_debug("zeiss_set_grids::level: %d, tile_w: %ld, tile_h: %ld", l, level->tile_w, level->tile_h);
    grid = _openslide_grid_create_range( osr,
                                         level->tile_w / level->downsample,
                                         level->tile_h / level->downsample,
                                         zeiss_tileread,
                                         (GDestroyNotify) czi_free_tile_descriptor );

    // Get tiles for the level
    GList * level_tiles = _openslide_czi_get_level_tiles(czi, l, err);
    GList * current_tile = level_tiles;
    //g_debug( "zeiss_set_grids::list of %d tiles for level %d", g_list_length( current_tile ), l );

    // Add level tiles to the level range grid
    while( current_tile ) {
      // Check that tile intersects region
      struct _openslide_czi_tile_descriptor * tile_desc = current_tile->data;
     // g_debug("zeiss_set_grids::tile_desc: %lf, %lf, %lf, %lf, %d, %d",
     //            (double)tile_desc->start_x, (double)tile_desc->start_y,
     //            (double)tile_desc->size_x, (double)tile_desc->size_y,
     //            tile_desc->subsampling_x, tile_desc->subsampling_y);
      _openslide_grid_range_add_tile( grid,
                                      (double)(tile_desc->start_x - offset_x) / level->downsample,
                                      (double)(tile_desc->start_y - offset_y) / level->downsample,
                                      (double)tile_desc->size_x / level->downsample,
                                      (double)tile_desc->size_y / level->downsample,
                                      tile_desc );
      current_tile  = g_list_next( current_tile );
    }

    g_list_free(level_tiles);

    // Mandatory Post processing call for range grid
    _openslide_grid_range_finish_adding_tiles(grid);

    downsample = czi_new_S32(level->downsample, err);
    g_hash_table_insert( czi->grids,
                         downsample,
                         grid );

  }

  return true;
}

// key:DRIVER-API-DEF
//============================================================================
//
//              ZEISS VENDOR - OPENSLIDE API : DEFINITIONS
//
//============================================================================

void zeiss_destroy(
  openslide_t * osr
)
{
  //g_debug("zeiss_destroy");
#ifdef CZI_DEBUG 
  // Display information about read tiles
  struct _czi                           * czi = (struct _czi *)osr->data;
  g_debug("-- Summary about read tiles --");
  zeiss_debug_display_tile_counts(osr, czi->tileread_counts, true);
  g_debug("-- Summary about cached tiles --");
  zeiss_debug_display_tile_counts(osr, czi->tilecached_counts, true);
#endif

  if( osr->data )
    _openslide_czi_free( (_openslide_czi *) osr->data );
  for( int32_t i = 0; i < osr->level_count; ++i )
    g_slice_free( struct _openslide_level, osr->levels[i] );
  g_free( osr->levels );
  return;
}

bool zeiss_paint_region(
  openslide_t               * osr    G_GNUC_UNUSED,
  cairo_t                   * cr     G_GNUC_UNUSED,
  int64_t                     x      G_GNUC_UNUSED,
  int64_t                     y      G_GNUC_UNUSED,
  struct _openslide_level   * level  G_GNUC_UNUSED,
  int32_t                     w      G_GNUC_UNUSED,
  int32_t                     h      G_GNUC_UNUSED,
  GError                   ** err    G_GNUC_UNUSED
)
{
  struct _czi * czi = (struct _czi *)osr->data;
  struct _openslide_grid * grid;
  bool success = false;
  // g_debug( "zeiss_paint_region" );

  int32_t l = _openslide_get_level_index(osr, level);
  if (l < 0) {
    // No matching level found
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find level for downsampling: %d", (int32_t)level->downsample );
    return false;
  }

  int32_t offset_x, offset_y;
  if (!_openslide_czi_get_level_tile_offset(czi, l, &offset_x, &offset_y, err)) {
    return false;
  }

  int32_t d = (int32_t)level->downsample;

  //g_debug( "zeiss_paint_region::d: %d, x: %ld, y: %ld, w: %d, h: %d, offset_x: %d, offset_y: %d", d, x, y, w, h, offset_x, offset_y );

  grid = g_hash_table_lookup(czi->grids, &d);
  if(!grid) {
    // No matching grid found for the downsampling
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to find grid for downsampling: %d", (int32_t)level->downsample );
    return false;
  }

  success = _openslide_grid_paint_region( grid, cr, NULL,
                                          x / d,
                                          y / d,
                                          level,
                                          w,
                                          h,
                                          err);

  return success;
}

bool zeiss_tileread(
  openslide_t               * osr,
  cairo_t                   * cr,
  struct _openslide_level   * level,
  int64_t                     tile_unique_id,
  void                      * tile,
  void                      * arg            G_GNUC_UNUSED,
  GError                   ** err
)
{
  //g_debug("zeiss_tileread");

  struct _czi * czi = (struct _czi *)osr->data;
  struct _openslide_czi_tile_descriptor * tile_desc;
  int32_t data_size = 0, internal_data_size = 0;
  uint8_t * tile_data, * internal_tile_data;
  int32_t l = _openslide_get_level_index(osr, level);
  cairo_format_t format = CAIRO_FORMAT_ARGB32;

  // Get tile descriptor from czi struct
  tile_desc = (struct _openslide_czi_tile_descriptor *)tile;

  if (!tile_desc) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Unable to get tile descriptor: %ld", tile_unique_id );
    return false;
  }
  
//   g_debug("zeiss_tileread::trying to get tile %ld at %d %d from cache",
//          tile_desc->uid,
//          tile_desc->start_x,
//          tile_desc->start_y);

  // Try to get tile data from cache
  struct _openslide_cache_entry *cache_entry;
  tile_data = (uint8_t *)_openslide_cache_get( osr->cache,
                                               level,
                                               tile_desc->start_x,
                                               tile_desc->start_y,
                                               &cache_entry );

  if (!tile_data) { 
#ifdef CZI_DEBUG
    // Increment read count for the tile
    int64_t * p_counts = (int64_t *)g_hash_table_lookup(
                                        czi->tileread_counts,
                                        czi_new_S64(tile_desc->uid, 0));
    
    if (!p_counts)
        g_hash_table_insert(czi->tileread_counts,
                            czi_new_S64(tile_desc->uid, 0),
                            czi_new_S64(1, 0));
    else
        (*p_counts)++;
#endif

//     g_debug("zeiss_tileread::reading tile %ld at %d %d from disk",
//          tile_desc->uid,
//          tile_desc->start_x,
//          tile_desc->start_y);

    // Load tile data from czi format (BGR_24, BGRA_32, ...)
    tile_data = _openslide_czi_get_level_tile_data( czi,
                                                    l,
                                                    tile_desc->uid,
                                                    &data_size,
                                                    err );
//     g_debug( "zeiss_tileread:: uid: %ld"
//              ", data_size: %d"
//              ", start_x: %d"
//              ", start_y: %d"
//              ", size_x: %d"
//              ", size_y: %d"
//              ", subsampling_x: %d"
//              ", subsampling_y: %d",
//              tile_desc->uid,
//              data_size,
//              tile_desc->start_x,
//              tile_desc->start_y,
//              tile_desc->size_x,
//              tile_desc->size_y,
//              tile_desc->subsampling_x,
//              tile_desc->subsampling_y );
    if (!tile_data) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unable to get data for tile uid: %ld", tile_desc->uid );

      return false;
    }

    // Uncompress tile data if needed
    if (tile_desc->compression != UNCOMPRESSED) {
      internal_tile_data = _openslide_czi_uncompress_tile( tile_desc,
                                                           tile_data,
                                                           data_size,
                                                           &internal_data_size,
                                                           err );
      //g_debug("zeiss_tileread:: uncompressed data size %d", internal_data_size);
      if (!internal_tile_data)
        return false;

      // Free loaded tile data
      _openslide_czi_free_level_tile_data( czi,
                                           l,
                                           tile_desc->uid,
                                           err );

      // Set uncompressed tile data information
      tile_data = internal_tile_data;
      data_size = internal_data_size;
    }
    /*
    g_debug("zeiss_tileread:: tile data %x, size %d",
            tile_data,
            data_size);
    */
    // Convert tile data to RGBA_32 buffer that is used in cairo
    internal_tile_data = _openslide_czi_data_convert_to_rgba32(
                                      tile_desc->pixel_type,
                                      tile_data,
                                      data_size,
                                      &internal_data_size,
                                      err );
    /*
    g_debug("zeiss_tileread:: converted tile data %x, new size %d",
            internal_tile_data,
            internal_data_size);
    
    g_debug("zeiss_tileread:: converted tile data %d, %d, %d, %d",
            *(internal_tile_data),
            *(internal_tile_data + 1),
            *(internal_tile_data + 2),
            *(internal_tile_data + 3)
           );
    */
    if (tile_desc->compression != UNCOMPRESSED) {
      // Free uncompressed tile data
      g_slice_free1( data_size, tile_data );
    }
    else {
      // Free loaded tile data
      _openslide_czi_free_level_tile_data( czi,
                                           l,
                                           tile_desc->uid,
                                           err );
    }
    /*
    g_debug("zeiss_tileread:: converted tile data after free %d, %d, %d, %d",
            *(internal_tile_data),
            *(internal_tile_data + 1),
            *(internal_tile_data + 2),
            *(internal_tile_data + 3)
           );
    */
    if (!internal_tile_data) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unable to convert data to cairo format for tile uid: %ld",
                  tile_desc->uid );

      return false;
    }

    // Set RGBA_32 converted tile data information
    tile_data = internal_tile_data;
    data_size = internal_data_size;

    // Put tile data in the cache
    // g_debug("Put tile %ld, level %d, x %d, y %d in cache",
    //        tile_desc->uid,
    //        l,
    //        (int32_t)tile_desc->start_x,
    //        (int32_t)tile_desc->start_y);
    _openslide_cache_put( osr->cache,
                          level, tile_desc->start_x, tile_desc->start_y,
                          tile_data, data_size,
                          &cache_entry );
  }
#ifdef CZI_DEBUG
  else {

//     g_debug("zeiss_tileread::retrieving tile %ld at %d %d from cache",
//          tile_desc->uid,
//          tile_desc->start_x,
//          tile_desc->start_y);
    
    // Increment cached count for the tile
    int64_t * p_counts = (int64_t *)g_hash_table_lookup(
                                        czi->tilecached_counts,
                                        czi_new_S64(tile_desc->uid, 0));
    
    if (!p_counts)
        g_hash_table_insert(czi->tilecached_counts,
                            czi_new_S64(tile_desc->uid, 0),
                            czi_new_S64(1, 0));
    else
        (*p_counts)++;
  }
#endif


  // Draw tile
  int32_t width = tile_desc->size_x / level->downsample;
  int32_t height = tile_desc->size_y / level->downsample;
  int32_t stride = cairo_format_stride_for_width (format, width);

  cairo_surface_t *surface = cairo_image_surface_create_for_data( tile_data,
                                                                  format,
                                                                  width,
                                                                  height,
                                                                  stride );
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // Done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

bool zeiss_detect(
  const char                  * filename,
  struct _openslide_tifflike  * tl G_GNUC_UNUSED,
  GError                     ** err
)
{
  //g_debug( "zeiss_detect" );
  if( !_openslide_czi_is_zisraw( filename, err ) )
    return false;

  // maybe check other things here ( xml, image encoding, ... )

  return true;
}

bool zeiss_open(
  openslide_t                 * osr,
  const char                  * filename,
  struct _openslide_tifflike  * tl          G_GNUC_UNUSED,
  struct _openslide_hash      * quickhash1  G_GNUC_UNUSED,
  GError                     ** err
)
{
  //g_debug( "zeiss_open" );
  _openslide_czi * czi_descriptor = _openslide_czi_decode( filename, err );

  if( !czi_descriptor )
    return false;

  if( !zeiss_check( czi_descriptor, err ) ) {
    _openslide_czi_free( czi_descriptor );
    return false;
  }

  if( !zeiss_set_properties( osr, czi_descriptor, err ) ){
    _openslide_czi_free( czi_descriptor );
    return false;
  }

  if( !zeiss_set_levels( osr, czi_descriptor, err ) ){
    _openslide_czi_free( czi_descriptor );
    return false;
  }
#if 0
  if( !zeiss_set_rois( osr, czi_descriptor, err)){
    _openslide_czi_free( czi_descriptor );
    return false;
  }
#endif
  if( !zeiss_set_grids( osr, czi_descriptor, err)){
    _openslide_czi_free( czi_descriptor );
    return false;
  }

  osr->data = (void*) czi_descriptor;
  osr->ops = &_openslide_ops_zeiss;


  int32_t ox, oy;
  if (!_openslide_czi_get_level_tile_offset(czi_descriptor, 0, &ox, &oy, err)) {
    return false;
  }

  //g_debug( "zeiss_open, ox: %d, oy: %d", ox, oy );

  return true;

  // Note:
  // Maybe I don't free enough stuff on failure. I don't know what's done
  // in openslide.c when backend "open" functions failed ?
  // Should I free all levels, properties, etc here ?
  //
  // After a little thinking, and looking into openslide.c
  // I guess properties will be destroyed at closing (hash table with free
  // function). However, levels should definitely be destroyed here.
}
