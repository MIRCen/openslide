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
//--- preprocessing stuff ----------------------------------------------------
// change this
#define CZI_ALIGNMENT      32
#define CZI_HEADER_SIZE    32
// keep it this way of use const char * ?
#define CZI_FILE           "ZISRAWFILE"
#define CZI_DIRECTORY      "ZISRAWDIRECTORY"
#define CZI_SUBBLOCK       "ZISRAWSUBBLOCK"
#define CZI_METADATA       "ZISRAWMETADATA"
#define CZI_ATTACH         "ZISRAWATTACH"
#define CZI_ATTDIR         "ZISRAWATTDIR"
#define CZI_DELETED        "DELETED"

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
//    PUBLIC METHODS
// ===========================================================================

// Looks for ZISRAW magic string
static bool _openslide_czi_is_zisraw( const char * filename, GError ** err );

// Setting of _openslide_czi structure
// Everything is read except data blocks (tiles data, xml blocks, attachments data)
static _openslide_czi * _openslide_czi_decode( const char * filename, GError ** err );

// A few precomputed characteristics to detect unsupported files
static bool _openslide_czi_is_multi_view( _openslide_czi * czi );
static bool _openslide_czi_is_multi_phase( _openslide_czi * czi );
static bool _openslide_czi_is_multi_block( _openslide_czi * czi );
static bool _openslide_czi_is_multi_illumination( _openslide_czi * czi );
static bool _openslide_czi_is_multi_rotation( _openslide_czi * czi );
static bool _openslide_czi_is_multi_time( _openslide_czi * czi );
static bool _openslide_czi_is_multi_zslice( _openslide_czi * czi );
static bool _openslide_czi_is_multi_channel( _openslide_czi * czi );
static bool _openslide_czi_has_data_uncompressed( _openslide_czi * czi );
static bool _openslide_czi_has_data_jpg( _openslide_czi * czi );
static bool _openslide_czi_has_data_jpgxr( _openslide_czi * czi );
static bool _openslide_czi_has_data_lzw( _openslide_czi * czi );
static bool _openslide_czi_has_data_cameraspec( _openslide_czi * czi );
static bool _openslide_czi_has_data_systemspec( _openslide_czi * czi );

// Tiles
/*TODO*/static int32_t   _openslide_czi_get_level_count( _openslide_czi * czi, GError **err );
/*TODO*/static GList   * _openslide_czi_get_level_tiles( _openslide_czi * czi, GError **err );
/*TODO*/static uint8_t * _openslide_czi_load_tile( _openslide_czi * czi, const char * guid, GError **err );
/*TODO*/static void      _openslide_czi_destroy_tile( _openslide_czi * czi, const char * guid, GError **err );

// Metadata
// There is one metadata block per file. In the multi-file case, I guess 
// the same metadata block is stored in each file.
// Still, we give the possibility to choose which metadata block to load.
// In all cases at least one metadata block is present, so calling the 
// load method with index 0 should always return something.
/*TODO*/static int32_t   _openslide_czi_get_metadata_count( _openslide_czi * czi, GError **err );
/*TODO*/static uint8_t * _openslide_czi_load_metadata( _openslide_czi * czi, int32_t index, GError **err );
/*TODO*/static void      _openslide_czi_destroy_metadata( _openslide_czi * czi, int32_t index ); // Or have the user do it ?

// Attachments
// If a null pointer is returned along with no error, it means that the 
// attachment is not stored in the file
/*TODO*/static _openslide_czi * _openslide_czi_decode_label( _openslide_czi * czi, GError **err );
/*TODO*/static _openslide_czi * _openslide_czi_decode_prescan( _openslide_czi * czi, GError **err );
/*TODO*/static _openslide_czi * _openslide_czi_decode_slide_preview( _openslide_czi * czi, GError **err );

// Free
// We give functions to free -openslide_czi structure,
// GList[ _openslide_czi_tile_descriptor ] and any loaded buffer.
// Buffers must be destroyed right after they are used : Freeing 
// _openslide_czi won't destroy thoses buffers !
static void _openslide_czi_free( _openslide_czi * czi );

// key:PARSING-PRI-DECL
//============================================================================
//
//               ZISRAW PARSING PRIVATE METHODS : DECLARATIONS
//
//============================================================================

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
  char                    uid[9];
  enum czi_pixel_t        pixel_type;
  enum czi_compression_t  compression;
  enum czi_pyramid_t      pyramid_type;
  GHashTable            * dimensions;   // key: char * - struct _czi_dimension
  int32_t                 directory_size;
  int32_t                 metadata_size;
  int32_t                 data_size;
  int32_t                 attachment_size;
  uint8_t               * metadata_buf;              // only loaded when asked
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
  int32_t               xml_size;
  int32_t               attachment_size;
  uint8_t             * xml_buf;
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
//    GENERIC UTILS (move them after)
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
  struct _czi_segment_header * header = (struct _czi_segment_header*) g_malloc0( sizeof(struct _czi_segment_header) );
  while( !feof( source->stream ) && (!check_position || position < position_max ) )
  {
    if( !czi_read_next_segment_header( source, header, err ) ) {
      // we assume it is because there are no segments left
      g_free( header );
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
      if( !czi_parse_attdir( source, czi, err ) )
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
  g_assert( czi );
  g_assert( tile );
  struct _czi_level * level;
  gpointer has_key;
  GList * list_keys, * current_key;
  int32_t start, size;
  int32_t * cur_start, * cur_size;

  for( uint32_t i=0; i < czi->levels->len; ++i )
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
  }

  g_hash_table_insert( level->tiles, g_strdup( (char*)tile->uid ), tile );
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
    current_key = g_list_next( current_key );
  }

  g_list_free( list_keys );
  return true;
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
  g_return_val_if_fail( err == NULL || *err == NULL, false );

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
  struct _czi * czi = (struct _czi*) g_try_malloc0( sizeof(struct _czi) );
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

  level->size = g_hash_table_new_full( &g_str_hash, &g_str_equal,
                &g_free, (void(*)(gpointer)) &czi_free_S32 );
  level->start = g_hash_table_new_full( &g_str_hash, &g_str_equal,
                 &g_free, (void(*)(gpointer)) &czi_free_S32 );
  level->tiles = g_hash_table_new_full( &g_str_hash, &g_str_equal,
                 &g_free, &g_free );

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
    g_slice_free1( sizeof(struct _czi_tile), tile );
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

//============================================================================
//   FREE
//============================================================================

void czi_free( struct _czi * ptr )
{
  if( ptr ) {
    if( ptr->sources )      g_ptr_array_free( ptr->sources, true );
    if( ptr->file_headers ) g_ptr_array_free( ptr->file_headers, true );
    if( ptr->levels )       g_ptr_array_free( ptr->levels, true );
    if( ptr->metadata )     g_ptr_array_free( ptr->metadata, true );
    if( ptr->attachments )  g_hash_table_destroy( ptr->attachments );
    g_free( ptr );
  }
}

void czi_free_source( struct _czi_source * ptr )
{
  if( ptr ) {
    if( ptr->filename ) g_free( ptr->filename );
    if( ptr->stream )   fclose( ptr->stream );
    g_slice_free1( sizeof(struct _czi_source), ptr );
  }
}

void czi_free_file_header( struct _czi_file_header * ptr )
{
  if( ptr ) g_slice_free1( sizeof(struct _czi_file_header), ptr );
}

void czi_free_level( struct _czi_level * ptr )
{
  if( ptr ) {
    if( ptr->size )       g_hash_table_destroy( ptr->size );
    if( ptr->start )      g_hash_table_destroy( ptr->start );
    if( ptr->tiles )      g_hash_table_destroy( ptr->tiles );
    g_slice_free1( sizeof(struct _czi_source), ptr );
  }
}

void czi_free_metadata( struct _czi_metadata * ptr )
{
  if( ptr ) g_slice_free1( sizeof(struct _czi_metadata), ptr );
}

void czi_free_attachment( struct _czi_attachment * ptr )
{
  if( ptr ) g_slice_free1( sizeof(struct _czi_attachment), ptr );
}

void czi_free_tile( struct _czi_tile * ptr )
{
  if( ptr ) {
    if( ptr->dimensions )  g_hash_table_destroy( ptr->dimensions );
    g_slice_free1( sizeof(struct _czi_tile), ptr );
  }
  return;
}

void czi_free_dimension( struct _czi_dimension * ptr )
{
  if( ptr ) g_slice_free1( sizeof(struct _czi_dimension), ptr );
}

void czi_free_S32( int32_t * ptr )
{
  if( ptr ) g_slice_free1( sizeof(int32_t), ptr );
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
  g_assert( source );
  g_assert( source->stream );
  g_assert( czi );

  FILE * stream = source->stream;
  int32_t entry_count;
  int32_t ss_x, ss_y;
  int32_t   val_S32;
  int32_t * ptr_S32;
  struct _czi_tile * new_tile = NULL;
  struct _czi_dimension * dim;

  TRY_READ_ITEMS( &entry_count, 1, sizeof(entry_count), stream, err, "Failed to parse directory: " );
  fseeko( stream, 124, SEEK_CUR );                       // 124 bytes reserved

  for( int32_t i=0; i<entry_count; ++i )
  {
    new_tile = czi_new_tile( err );
    if( !new_tile ) return false;
    if( !czi_read_tile( source, new_tile, err ) ) {
      czi_free_tile( new_tile );
      return false;
    }
    new_tile->uid[8] = '\0';

    dim = (struct _czi_dimension*) g_hash_table_lookup( new_tile->dimensions, "X" );
    if( !dim ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Tile without X dimension." );
      czi_free_tile( new_tile );
      return false;
    }
    ss_x = dim->size / dim->stored_size;
    val_S32 = dim->start;
    ptr_S32 = (int32_t*) new_tile->uid;
    *ptr_S32 = val_S32;

    dim = (struct _czi_dimension*) g_hash_table_lookup( new_tile->dimensions, "Y" );
    if( !dim ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Tile without Y dimension." );
      czi_free_tile( new_tile );
      return false;
    }
    ss_y = dim->size / dim->stored_size;
    val_S32 = dim->start;
    ptr_S32 = (int32_t*) (new_tile->uid+4);
    *ptr_S32 = val_S32;

    if( !czi_add_tile( czi, new_tile, ss_x, ss_y, err ) ) {
      czi_free_tile( new_tile );
      return false;
    }
    new_tile = NULL;
  }

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
  g_assert( source );
  g_assert( source->stream );
  g_assert( metadata );

  metadata->source = source;
  FILE * stream = source->stream;

  TRY_READ_ITEMS( &(metadata->xml_size),        1, 4, stream, err, "Failed to read metadata: " );
  TRY_READ_ITEMS( &(metadata->attachment_size), 1, 4, stream, err, "Failed to read metadata: " );
  TRY_FSEEKO( stream, 248,                       SEEK_CUR, err, "Failed to read metadata: " );  // end of segment
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
  tile->pixel_type = (enum czi_pixel_t) val32;
  TRY_READ_ITEMS( &(tile->tile_offset), 1, 8, stream, err, "Failed to read tile: " );
  TRY_READ_ITEMS( &(tile->file_part),   1, 4, stream, err, "Failed to read tile: " );
  TRY_READ_ITEMS( &val32, 1, sizeof(int32_t), stream, err, "Failed to read tile: " );    // Compression
  tile->compression = (enum czi_compression_t) val32;
  TRY_READ_ITEMS( &val8,                1, 1, stream, err, "Failed to read tile: " );    // PyramidType
  tile->pyramid_type = (enum czi_pyramid_t) val8;
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
int32_t _openslide_czi_get_level_count( _openslide_czi * czi, GError **err )
{
  // TODO
  return 0;
}

GList   * _openslide_czi_get_level_tiles( _openslide_czi * czi, GError **err )
{
  // TODO
  return NULL;
}

uint8_t * _openslide_czi_load_tile( _openslide_czi * czi, const char * guid, GError **err )
{
  // TODO
  return NULL;
}

void _openslide_czi_destroy_tile( _openslide_czi * czi, const char * guid, GError **err )
{
  // TODO
  return;
}

//--- metadata ---------------------------------------------------------------
int32_t _openslide_czi_get_metadata_count( _openslide_czi * czi, GError **err )
{
  return 0;
}

uint8_t * _openslide_czi_load_metadata( _openslide_czi * czi, int32_t index, GError **err )
{
  return NULL;
}

void _openslide_czi_destroy_metadata( _openslide_czi * czi, int32_t index )
{
  return;
}

//--- attachments ------------------------------------------------------------
_openslide_czi * _openslide_czi_decode_label( _openslide_czi * czi, GError **err )
{
  return NULL;
}

_openslide_czi * _openslide_czi_decode_prescan( _openslide_czi * czi, GError **err )
{
  return NULL;
}

_openslide_czi * _openslide_czi_decode_slide_preview( _openslide_czi * czi, GError **err )
{
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

// key:DRIVER-PRI-DECL
//============================================================================
//
//              ZEISS VENDOR - PRIVATE METHODS : DECLARATIONS
//
//============================================================================

static bool zeiss_set_properties(
  openslide_t     * osr,
  _openslide_czi  * czi_descriptor,
  GError         ** err
);

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

bool zeiss_set_properties(
  openslide_t     * osr,
  _openslide_czi  * czi_descriptor,
  GError         ** err
)
{
  int32_t meta_count = _openslide_czi_get_metadata_count( czi_descriptor, err );
  if( meta_count <= 0 ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "No metadata block to load" );
    return false;
  }

  // try load metadata buffer n0
  // (we suppose all files contain the same xml block)
  uint8_t * xml_buffer = NULL;
  xml_buffer = _openslide_czi_load_metadata( czi_descriptor, 0, err );
  if( !xml_buffer )
    return false;

  // try convert xml
  xmlDoc * xml_doc = NULL;
  xml_doc = _openslide_xml_parse( (char*) xml_buffer, err );
  _openslide_czi_destroy_metadata( czi_descriptor, 0 );
  if( !xml_doc ) {
    return false;
  }

  // get path context
  xmlXPathContext * xml_path_context = _openslide_xml_xpath_create( xml_doc );

  // set openslide keys
  _openslide_xml_set_prop_from_xpath( osr, xml_path_context,
    "zeiss.scaling.items.distance-x.value",
    "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='X']/Value" );

  _openslide_xml_set_prop_from_xpath( osr, xml_path_context,
    "zeiss.scaling.items.distance-x.default-unit-format",
    "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='X']/DefaultUnitFormat" );

  _openslide_xml_set_prop_from_xpath( osr, xml_path_context,
    "zeiss.scaling.items.distance-y.value",
    "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='Y']/Value" );

  _openslide_xml_set_prop_from_xpath( osr, xml_path_context,
    "zeiss.scaling.items.distance-y.default-unit-format",
    "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='Y']/DefaultUnitFormat" );
  
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
  return;
}

bool zeiss_paint_region(
  openslide_t               * osr,
  cairo_t                   * cr,
  int64_t                     x,
  int64_t                     y,
  struct _openslide_level   * level,
  int32_t                     w,
  int32_t                     h,
  GError                   ** err
)
{
  return true;
}

bool zeiss_tileread(
  openslide_t               * osr,
  cairo_t                   * cr,
  struct _openslide_level   * level,
  int64_t                     tile_col,
  int64_t                     tile_row,
  void                      * arg,
  GError                   ** err
)
{
  // For simple grid
  // Do we need this ? Our brightfields images are registered tile-wise.
  // Are there images where tile positions are kept natural ?
  return true;
}

bool zeiss_tilemap(
  openslide_t               * osr,
  cairo_t                   * cr,
  struct _openslide_level   * level,
  int64_t                     tile_col,
  int64_t                     tile_row,
  void                      * tile,
  void                      * arg,
  GError                   ** err
)
{
  return true;
}

void zeiss_tilemap_foreach(
  struct _openslide_grid  * grid,
  int64_t                   tile_col,
  int64_t                   tile_row,
  void                    * tile,
  void                    * arg
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
  // ensure we have a zisraw file
  if( !_openslide_czi_is_zisraw( filename, err ) )
    return false;

  // maybe check other things here ( xml, image encoding, ... )

  return true;
}

bool zeiss_open(
  openslide_t                 * osr,
  const char                  * filename,
  struct _openslide_tifflike  * tl G_GNUC_UNUSED,
  struct _openslide_hash      * quickhash1,
  GError                     ** err
)
{
  g_debug( "zeiss_open" );
  _openslide_czi * czi_descriptor = NULL;
  czi_descriptor = _openslide_czi_decode( filename, err );
  if( !czi_descriptor )
    return false;
  osr->data = (void*) czi_descriptor;

  // try read metadata
  if( !zeiss_set_properties( osr, (_openslide_czi*) osr->data, err ) )
    return false;

  osr->level_count = _openslide_czi_get_level_count( (_openslide_czi*) osr->data, 0 );


  return true;
}