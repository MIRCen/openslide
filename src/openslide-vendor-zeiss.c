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
 *          - ZISRAW PRIVATE METHODS DEFINITIONS          key:PARSING-PUB-DEF
 *          - ZISRAW PUBLIC API DEFINITIONS               key:PARSING-PUB-DEF
 *   II) ZEISS-VENDOR DRIVER                              key:DRIVER-TOP
 *       1) DECLARATIONS
 *          - OPENSLIDE API DECLARATIONS                  key:DRIVER-API-DECL
 *          - PRIVATE METHODS DECLARATIONS                key:DRIVER-PRI-DECL
 *       2) DEFINITIONS
 *          - PRIVATE METHODS DECLARATIONS                key:DRIVER-API-DEF
 *          - OPENSLIDE API DEFINITIONS                   key:DRIVER-PRI-DEF
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
#include "openslide-private.h"
//--- extern -----------------------------------------------------------------
// specify which is needed for ZISRAW ".h", ZISRAW ".c", ZEISS ".c"
#include <glib.h>
#include <stdint.h>                                      // c99 int data types
#include <stdbool.h>                                                   // bool
#include <stdio.h>                                            // file handling
#include <errno.h>                                      // file handling error
#include <string.h>                                       // string comparison
#include <sys/types.h>                                   // int MIN/MAX values
//----------------------------------------------------------------------------

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

// ===========================================================================
//    PUBLIC STRUCTURES
// ===========================================================================

// Descriptive structure used to navigate in the czi file.
// Its characteristics should be hidden to the user.
typedef struct _czi _openslide_czi;

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

// ===========================================================================
//    PUBLIC METHODS
// ===========================================================================

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
static bool _openslide_czi_has_data_jpgxr( _openslide_czi * czi );
static bool _openslide_czi_has_data_lzw( _openslide_czi * czi );
static bool _openslide_czi_has_data_cameraspec( _openslide_czi * czi );
static bool _openslide_czi_has_data_systemspec( _openslide_czi * czi );

// Tiles
static int32_t   _openslide_czi_get_level_count( _openslide_czi * czi );
static int32_t   _openslide_czi_get_level_subsampling( _openslide_czi * czi, int32_t level, GError **err );
static bool      _openslide_czi_get_level_tile_size( _openslide_czi * czi, int32_t level, int32_t * w, int32_t * h, GError ** err );
static GList   * _openslide_czi_get_level_tiles( _openslide_czi * czi, int32_t level, GError **err );
static void      _openslide_czi_free_list_tiles( GList * list );
static uint8_t * _openslide_czi_load_tile( _openslide_czi * czi, int32_t level, int64_t uid, int32_t * buffer_size, GError **err );
static bool      _openslide_czi_destroy_tile( _openslide_czi * czi, int32_t level, int64_t uid, GError **err );

// Metadata
// There is one metadata block per file. In the multi-file case, I guess 
// the same metadata block is stored in each file.
// Still, we give the possibility to choose which metadata block to load.
// In all cases at least one metadata block is present, so calling the 
// load method with index 0 should always return something.
static int32_t   _openslide_czi_get_metadata_count( _openslide_czi * czi );
static char    * _openslide_czi_load_metadata( _openslide_czi * czi, int32_t index, int32_t * buffer_size, GError **err );
static bool      _openslide_czi_destroy_metadata( _openslide_czi * czi, int32_t index, GError **err );

// Attachments
// If a null pointer is returned along with no error, it means that the 
// attachment is not stored in the file
/*TODO*/static _openslide_czi * _openslide_czi_decode_label( _openslide_czi * czi, GError **err );
/*TODO*/static _openslide_czi * _openslide_czi_decode_prescan( _openslide_czi * czi, GError **err );
/*TODO*/static _openslide_czi * _openslide_czi_decode_slide_preview( _openslide_czi * czi, GError **err );

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
  GPtrArray   * sources;                                 // struct _czi_source
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
  GPtrArray   * levels;                                   // struct _czi_level
  GPtrArray   * metadata;                              // struct _czi_metadata
  GHashTable  * attachments;      // key: guid - value: struct _czi_attachment
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
  GHashTable            * tiles;        // key: guid - value: struct _czi_tile
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

//--- read _czi --------------------------------------------------------------
static bool czi_find_sources( const char * filename, struct _czi * czi, GError ** err );
static bool czi_decode_one_stream( struct _czi_source * source, struct _czi * czi, GError ** err );
static bool czi_add_tile( struct _czi * czi, struct _czi_tile * tile, int32_t ss_x, int32_t ss_y, GError ** err );
static bool czi_update_bool_dimension( struct _czi * czi, char key, int32_t size, GError ** err );
static bool czi_update_bool_compression( struct _czi * czi, enum czi_compression_t compression, GError ** err );
static int32_t czi_cmp_level( const struct _czi_level ** l1, const struct _czi_level ** l2 );

//--- new --------------------------------------------------------------------
static struct _czi             * czi_new( GError ** err );
static struct _czi_source      * czi_new_source( GError ** err );
static struct _czi_file_header * czi_new_file_header( struct _czi * czi, GError ** err );
static struct _czi_level       * czi_new_level( struct _czi * czi, GError ** err);
static struct _czi_metadata    * czi_new_metadata( struct _czi * czi, GError ** err );
/*TODO*/static struct _czi_attachment  * czi_new_attachment( struct _czi * czi, GError ** err );
static struct _czi_tile        * czi_new_tile( GError ** err );
static struct _czi_dimension   * czi_new_dimension( struct _czi_tile * tile, GError ** err );
static int32_t                 * czi_new_S32( int32_t integer, GError ** err );
static int64_t                 * czi_new_S64( int64_t integer, GError ** err );
static struct _openslide_czi_tile_descriptor * czi_new_tile_descriptor( struct _czi_tile * tile, GError ** err );

//--- free -------------------------------------------------------------------
static void czi_free(              struct _czi              * ptr );
static void czi_free_source(       struct _czi_source       * ptr );
static void czi_free_file_header(  struct _czi_file_header  * ptr );
static void czi_free_level(        struct _czi_level        * ptr );
static void czi_free_metadata(     struct _czi_metadata     * ptr );
static void czi_free_attachment(   struct _czi_attachment   * ptr );
static void czi_free_tile(         struct _czi_tile         * ptr );
static void czi_free_dimension(    struct _czi_dimension    * ptr );
static void czi_free_S32(          int32_t                  * ptr );
static void czi_free_S64(          int64_t                  * ptr );
static void czi_free_tile_descriptor( struct _openslide_czi_tile_descriptor * ptr );

//--- read -------------------------------------------------------------------
static bool czi_parse_directory(  struct _czi_source * source, struct _czi * czi,                     GError ** err );
/*TODO*/static bool czi_parse_attdir(     struct _czi_source * source, struct _czi * czi,                     GError ** err );
static bool czi_read_file_header( struct _czi_source * source, struct _czi_file_header * file_header, GError ** err );
static bool czi_read_metadata(    struct _czi_source * source, struct _czi_metadata * metadata,       GError ** err );
static bool czi_read_tile(        struct _czi_source * source, struct _czi_tile * tile,               GError ** err );
static bool czi_read_dimension(   struct _czi_source * source, struct _czi_dimension * dimension,     GError ** err );

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
);
static bool czi_skip_segment(
  struct _czi_source          * source,
  struct _czi_segment_header  * segmentheader,
  GError                     ** err
);
static bool czi_is_zisraw(
  FILE                        * stream,
  GError                     ** err
);

// key:PARSING-PUB-DEF
//============================================================================
//
//                        PRIVATE API DEFINITIONS
//
//============================================================================

//============================================================================
//    GENERIC UTILS (move them after ?)
//============================================================================

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

//============================================================================
//   READ CZI
//============================================================================

bool czi_find_sources(
  const char   * filename,
  struct _czi  * czi,
  GError      ** err
)
{
  g_debug( "czi_find_sources" );
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
  g_debug( "czi_decode_one_stream" );
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

  int64_t position = ftello( source->stream );
  int64_t position_max = source->begin + source->size;
  bool check_position = ( source->size > 0 );
  struct _czi_segment_header * header = (struct _czi_segment_header*) g_slice_alloc0( sizeof(struct _czi_segment_header) );
  while( !feof( source->stream ) && (!check_position || position < position_max ) )
  {
    if( !czi_read_next_segment_header( source, header, err ) ) {
      // we assume it is because there are no segments left
      g_slice_free( struct _czi_segment_header, header );
      g_clear_error( err );
      err = NULL;
      break;
    }

    // Treat different cases
    if( !strcmp( header->id, CZI_FILE ) )
    {
      struct _czi_file_header * new_file_header = czi_new_file_header( czi, err );
      if( !new_file_header )
        return false;
      g_ptr_array_add( czi->file_headers, new_file_header );
      if( !czi_read_file_header( source, new_file_header, err ) )
        return false;
    }
    else if( !strcmp( header->id, CZI_DIRECTORY ) )
    {
      if( !czi_parse_directory( source, czi, err ) )
        return false;
    }
    else if( !strcmp( header->id, CZI_METADATA ) )
    {
      struct _czi_metadata * new_metadata = czi_new_metadata( czi, err );
      if( !new_metadata )
        return false;
      g_ptr_array_add( czi->metadata, new_metadata );
      if( !czi_read_metadata( source, new_metadata, err ) )
        return false;
    }
    else if( !strcmp( header->id, CZI_ATTDIR ) )
    {
      //if( !czi_parse_attdir( source, czi, err ) )
      if( !czi_skip_segment( source, header, err ) ) // Temporary
        return false;
    }
    else if( !strcmp( header->id, CZI_ATTACH ) )
    {
      if( !czi_skip_segment( source, header, err ) )
        return false;
    }
    else if( !strcmp( header->id, CZI_SUBBLOCK ) )
    {
      if( !czi_skip_segment( source, header, err ) )
        return false;
    }
    else if( !strcmp( header->id, CZI_DELETED ) )
    {
      if( !czi_skip_segment( source, header, err ) )
        return false;
    }
    else
    {
      // unexptected segment
      // try skip anyway...
      g_warning( "Unexpected segment %s", header->id );;
      if( !czi_skip_segment( source, header, err ) )
        return false;
    }
  }

  return true;
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
  struct _czi_level * level;
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

  // int32_t start_x = ( (struct _czi_dimension *) g_hash_table_lookup( tile->dimensions, "X" ) )->start;
  // int32_t start_y = ( (struct _czi_dimension *) g_hash_table_lookup( tile->dimensions, "Y" ) )->start;
  // g_debug( "insert l %d t %ld p (%d, %d)", ss_x, tile->uid, start_x, start_y );
  g_hash_table_insert( level->tiles, czi_new_S64(tile->uid,0), tile );
  list_keys = g_hash_table_get_keys( tile->dimensions );
  current_key = list_keys;
  while( current_key )
  {
    start = ((struct _czi_dimension*)g_hash_table_lookup( tile->dimensions, (char*) current_key->data ))->start;
    size  = ((struct _czi_dimension*)g_hash_table_lookup( tile->dimensions, (char*) current_key->data ))->size;
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
    if( current_pos == previous_pos ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): We're not moving in file anymore. "
                   "Better break the loop and go to end of file. "
                   "At %li.",
                   __FILE__, __LINE__, __func__, ftello(stream) );
      fseeko( stream, 0, SEEK_END );
      return false;
    }

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
  g_debug( "czi_is_zisraw" );
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
  czi->levels       = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_level );
  czi->metadata     = g_ptr_array_new_with_free_func( (void(*)(gpointer)) &czi_free_metadata );
  //czi->attachments  = g_hash_table_new_full( (void(*)(gpointer)) &czi_free_attachment );

  if( !czi->sources  || ! czi->file_headers || !czi->levels ||
      !czi->metadata /*|| !czi->attachments*/ ) {
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

//============================================================================
//   FREE
//============================================================================

void czi_free( struct _czi * ptr )
{
  // g_debug( "czi_free" );
  if( ptr ) {
    if( ptr->sources )      g_ptr_array_free( ptr->sources, true );
    if( ptr->file_headers ) g_ptr_array_free( ptr->file_headers, true );
    if( ptr->levels )       g_ptr_array_free( ptr->levels, true );
    if( ptr->metadata )     g_ptr_array_free( ptr->metadata, true );
    if( ptr->attachments )  g_hash_table_destroy( ptr->attachments );
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

void czi_free_tile_descriptor( struct _openslide_czi_tile_descriptor * ptr )
{
  // g_debug( "czi_free_tile_descriptor" );
  if( ptr ) g_slice_free( struct _openslide_czi_tile_descriptor, ptr );
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
  g_debug( "czi_parse_directory" );
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
  g_debug ( "czi_read_file_header" );
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
  g_debug( "czi_read_metadata" );
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
  // g_debug( "czi_read_dimension" );
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

// key:PARSING-PRI-DEF
//============================================================================
//
//                  ZISRAW PARSING PUBLIC API: DEFINITIONS
//
//============================================================================

//--- check ------------------------------------------------------------------
bool _openslide_czi_is_zisraw( const char * filename, GError ** err )
{
  g_debug( "_openslide_czi_is_zisraw" );
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
  g_debug( "_openslide_czi_decode" );
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
  struct _czi_tile  * tile = (struct _czi_tile *) g_hash_table_lookup( s_level->tiles, (const char *)keys->data );
  g_list_free( keys );
  if( !tile ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to load tile from level %d", level );
    return false;
  }
  struct _czi_dimension * dim = (struct _czi_dimension *) g_hash_table_lookup( tile->dimensions, "X" );
  if( !tile ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to load X dimension from level %d", level );
    return false;
  }
  *w = dim->stored_size;
  dim = (struct _czi_dimension *) g_hash_table_lookup( tile->dimensions, "Y" );
  if( !tile ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to load Y dimension from level %d", level );
    return false;
  }
  *h = dim->stored_size;

  return true;

}

GList * _openslide_czi_get_level_tiles(
  _openslide_czi  * czi,
  int32_t           i,
  GError         ** err
)
{
  struct _czi_level * level;
  struct _czi_tile * tile;
  struct _openslide_czi_tile_descriptor * tile_desc;
  GList * intern_list, * intern_list_current, * extern_list;

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
  TRY_FSEEKO( stream, tile->tile_offset, SEEK_SET, err, "Failed to load tile: " );
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

  if( buffer_size )  *buffer_size = tile->data_size;
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
  int64_t                     tile_col,
  int64_t                     tile_row,
  void                      * arg,
  GError                   ** err
);

static bool zeiss_tilemap(
  openslide_t               * osr,
  cairo_t                   * cr,
  struct _openslide_level   * level,
  int64_t                     tile_col,
  int64_t                     tile_row,
  void                      * tile,
  void                      * arg,
  GError                   ** err
);

static void zeiss_tilemap_foreach(
  struct _openslide_grid  * grid,
  int64_t                   tile_col,
  int64_t                   tile_row,
  void                    * tile,
  void                    * arg
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

static bool zeiss_check( _openslide_czi * czi, GError ** err );
static bool zeiss_set_properties( openslide_t * osr, _openslide_czi * czi, GError ** err );
static bool zeiss_set_levels( openslide_t * osr, _openslide_czi * czi, GError ** err );

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
#define ZEISS_OVERLAP            "zeiss.experiment.acquisition-block[%d].subdimension-setups.region-setup.sample-holder.overlap:"
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

#define ZEISS_SET_PROP( osr, context, property_format, path_format, num )    \
  {                                                                          \
    char *property, *path;                                                   \
    property = g_strdup_printf( property_format, num );                      \
    path = g_strdup_printf( path_format, num+1 );                            \
    _openslide_xml_set_prop_from_xpath( osr, context, property, path );      \
    g_free( property );                                                      \
    g_free( path );                                                          \
  }                                                                          \
  (void)0

#define ZEISS_SET_PROP2( osr, context, property_format, path_format, n1, n2 ) \
  {                                                                           \
    char *property, *path;                                                    \
    property = g_strdup_printf( property_format, n1, n2 );                    \
    path = g_strdup_printf( path_format, n1+1, n2+1 );                        \
    _openslide_xml_set_prop_from_xpath( osr, context, property, path );       \
    g_free( property );                                                       \
    g_free( path );                                                           \
  }                                                                           \
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
  if( _openslide_czi_has_data_jpgxr( czi ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "JPEGXR compression not supported" );
    return false;
  }
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

#ifdef ZEISS_WRITE_XML
  FILE * streamout = _openslide_fopen( "/tmp/zeiss.xml", "wb", 0 );
  fwrite( xml_buffer, xml_size, 1, streamout );
  fclose( streamout );
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
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/@Name", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_PIXEL_TYPE,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/PixelType", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_BIT_COUNT,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/ComponentBitcount", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_ACQMODE,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/AcquisitionMode", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_ILTYPE,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/IlluminationType", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_CONTRAST,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/ContrastMethod", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_FLUOR,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/Fluor", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_COLOR,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/Color", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_EXPTIME,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
      "/ExposureTime", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_CH_THCK,
      "/ImageDocument/Metadata/Information/Image/Dimensions/Channels/Channel[%d]"
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
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d]"
      "/ObjectiveName", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_LENSNA,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d]"
      "/LensNA", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_MAGN,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d]"
      "/NominalMagnification", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_DIST,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d]"
      "/WorkingDistance", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_GEOM,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d]"
      "/PupilGeometry", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_OBJ_IMMERSION,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective[%d]"
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
      "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
      "/SubDimensionSetups/RegionsSetup/SampleHolder/Overlap", i );
    ZEISS_SET_PROP( osr, xml_path_context, ZEISS_COVERING_MODE,
      "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
      "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegionCoveringMode", i );

    int32_t region_count = 0;
    char * path = g_strdup_printf(
      "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
      "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion", i+1 );
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
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_CENTER,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/CenterPosition", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_CONTOUR,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/ContourSize", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_COLUMNS,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/Columns", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_ROWS,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/Rows", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_Z,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/Z", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_ACQ,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/IsUsedForAcquisition", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_PROTECT,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/IsProtected", i, j );
      ZEISS_SET_PROP2( osr, xml_path_context, ZEISS_TILEREGION_CTYPE,
        "/ImageDocument/Metadata/Experiment/ExperimentBlocks/AcquisitionBlock[%d]"
        "/SubDimensionSetups/RegionsSetup/SampleHolder/TileRegions/TileRegion[%d]"
        "/Contour/@Type", i, j );
    }
  }

  xmlFreeDoc( xml_doc );
  xmlXPathFreeContext( xml_path_context );

  //--- set openslide properties ---------------------------------------------

  double mpp;
  mpp = 0;
  mpp = _openslide_parse_double( (const char*)g_hash_table_lookup( osr->properties, ZEISS_VOXELSIZE_X ) );
  mpp *= 1e6;
  g_hash_table_insert( osr->properties, g_strdup( OPENSLIDE_PROPERTY_NAME_MPP_X ), _openslide_format_double(mpp) );

  mpp = 0;
  mpp = _openslide_parse_double( (const char*)g_hash_table_lookup( osr->properties, ZEISS_VOXELSIZE_Y ) );
  mpp *= 1e6;
  g_hash_table_insert( osr->properties, g_strdup( OPENSLIDE_PROPERTY_NAME_MPP_Y ), _openslide_format_double(mpp) );

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
    w = g_hash_table_lookup( osr->properties, ZEISS_VOXELSIZE_X );
    level->w = (int64_t)( _openslide_parse_double( w ) / level->downsample );
    h = g_hash_table_lookup( osr->properties, ZEISS_VOXELSIZE_Y );
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
  if( osr->data )
    _openslide_czi_free( (_openslide_czi *) osr->data );
  for( int32_t i=0; i<osr->level_count; ++i )
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
  return true;
}

bool zeiss_tileread(
  openslide_t               * osr       G_GNUC_UNUSED,
  cairo_t                   * cr        G_GNUC_UNUSED,
  struct _openslide_level   * level     G_GNUC_UNUSED,
  int64_t                     tile_col  G_GNUC_UNUSED,
  int64_t                     tile_row  G_GNUC_UNUSED,
  void                      * arg       G_GNUC_UNUSED,
  GError                   ** err       G_GNUC_UNUSED
)
{
  // For simple grid
  // Do we need this ? Our brightfields images are registered tile-wise.
  // Are there images where tile positions are kept natural ?
  return true;
}

bool zeiss_tilemap(
  openslide_t               * osr       G_GNUC_UNUSED,
  cairo_t                   * cr        G_GNUC_UNUSED,
  struct _openslide_level   * level     G_GNUC_UNUSED,
  int64_t                     tile_col  G_GNUC_UNUSED,
  int64_t                     tile_row  G_GNUC_UNUSED,
  void                      * tile      G_GNUC_UNUSED,
  void                      * arg       G_GNUC_UNUSED,
  GError                   ** err       G_GNUC_UNUSED
)
{
  // For mapped grid
  return true;
}

void zeiss_tilemap_foreach(
  struct _openslide_grid  * grid      G_GNUC_UNUSED,
  int64_t                   tile_col  G_GNUC_UNUSED,
  int64_t                   tile_row  G_GNUC_UNUSED,
  void                    * tile      G_GNUC_UNUSED,
  void                    * arg       G_GNUC_UNUSED
)
{
  // I don't know if I need this
  return;
}

bool zeiss_detect(
  const char                  * filename,
  struct _openslide_tifflike  * tl G_GNUC_UNUSED,
  GError                     ** err
)
{
  g_debug( "zeiss_detect" );
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
  g_debug( "zeiss_open" );
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

// testouille diverse
#if 0
  osr->level_count = _openslide_czi_get_level_count( czi_descriptor );
  printf( "nb levels: %d\n", osr->level_count );
  int32_t level_ss1 = 0;
  for( int32_t i=0; i< osr->level_count; ++i ) {
    printf( "level %d: subsampling %d\n", i, _openslide_czi_get_level_subsampling( czi_descriptor, i, 0 ) );
    if( _openslide_czi_get_level_subsampling( czi_descriptor, i, 0 ) == 1 )
      level_ss1 = i;
  }

  GList * list_tiles = _openslide_czi_get_level_tiles( czi_descriptor, level_ss1, err );
  if( !list_tiles ) return false;
  int32_t tile_count = 0;
  GList * current = list_tiles; // struct _openslide_czi_tile_descriptor * tile;
  while( current )
  {
    // tile = (struct _openslide_czi_tile_descriptor *) current->data;
    // printf( "-----------------------------------------------------------\n" );
    // printf( "uid: %ld, pixel_type: %d, compression: %d, pyramid_type: %d\n",
    //   tile->uid, tile->pixel_type, tile->compression, tile->pyramid_type );
    // printf( "ss_x: %d, ss_y: %d, start_x: %d, start_y: %d, size_x: %d, size_y: %d\n",
    //   tile->subsampling_x, tile->subsampling_y, tile->start_x,
    //   tile->start_y, tile->size_x, tile->size_y );
    tile_count++;
    current = g_list_next( current );
  }
  // printf( "-----------------------------------------------------------\n" );
  printf( "nb tiles in level %d: %d\n", level_ss1, tile_count );

  // TODO: compute grid using xml properties and tile sizes

  _openslide_czi_free_list_tiles( list_tiles );
#endif

  osr->data = (void*) czi_descriptor;
  osr->ops = &_openslide_ops_zeiss;

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