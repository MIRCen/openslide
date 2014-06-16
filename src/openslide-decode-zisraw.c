/*
 * ZEISS (czi) zisraw support
 * Yaël Balbastre
 */

//--- openslide --------------------------------------------------------------
#include <config.h>       // I guess it's better ton include config in sources
#include "openslide-decode-zisraw.h"
#include "openslide-private.h"             // _openslide_fopen and other utils
//--- std --------------------------------------------------------------------
#include <string.h>
#include <stdio.h>                                                     // FILE
#include <errno.h>                                                    // errno
#include <sys/types.h>                                                // off_t
//--- preprocessing stuff ----------------------------------------------------
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
//----------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////////
///                                                                        ///
///            Z I S R A W   S T R U C T U R E   D E C O D I N G           ///
///                                                                        ///
//////////////////////////////////////////////////////////////////////////////

//============================================================================
//
//                            PRIVATE STRUCTURES
//
//============================================================================

//============================================================================
//   HELPER STRUCTURES (PRIVATE)
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

//============================================================================
//   GENERAL CZI STRUCTURES (PRIVATE)
//============================================================================

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
  uint8_t                 guid[16];
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
  char               dimension_id[4];
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
//   ATTACHMENT CONTENTS
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
//
//                       PRIVATE API DECLARATIONS
//
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
  uint8_t  * items,    // Pointer to memory block to process
  uint64_t   count,    // Number of items in the array
  size_t     size,     // Size of one item
  FILE     * stream    // File stream
);

//--- read _czi --------------------------------------------------------------
static bool czi_find_sources( const char * filename, struct _czi * czi, GError ** err );
static bool czi_decode_one_stream( struct _czi_source * source, struct _czi * czi, GError ** err );
/*TODO*/ static bool czi_add_tile( struct _czi * czi, struct _czi_tile * tile, int32_t ss_x, int32_t ss_y, GError ** err );

//--- new --------------------------------------------------------------------
static struct _czi             * czi_new( GError ** err );
static struct _czi_file_header * czi_new_file_header( struct _czi * czi, GError ** err );
static struct _czi_metadata    * czi_new_metadata( struct _czi * czi, GError ** err );
/*TODO*/ static struct _czi_tile        * czi_new_tile( GError ** err );

//--- free -------------------------------------------------------------------
static void czi_free(              struct _czi              * ptr );
static void czi_free_source(       struct _czi_source       * ptr );
static void czi_free_file_header(  struct _czi_file_header  * ptr );
static void czi_free_level(        struct _czi_level        * ptr );
static void czi_free_metadata(     struct _czi_metadata     * ptr );
static void czi_free_attachment(   struct _czi_attachment   * ptr );
/*TODO*/ static void czi_free_tile(         struct _czi_tile         * ptr );

//--- read -------------------------------------------------------------------
static bool czi_parse_directory(  struct _czi_source * source, struct _czi * czi,                     GError ** err );
/*TODO*/ static bool czi_parse_attdir(     struct _czi_source * source, struct _czi * czi,                     GError ** err );
static bool czi_read_file_header( struct _czi_source * source, struct _czi_file_header * file_header, GError ** err );
static bool czi_read_metadata(    struct _czi_source * source, struct _czi_metadata * metadata,       GError ** err );
/*TODO*/ static bool czi_read_tile(        struct _czi_source * source, struct _czi_tile * tile,               GError ** err );
/*TODO*/ static bool czi_read_dimension(   struct _czi_source * source, struct _czi_dimension * dimension,     GError ** err );

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

//--- print ------------------------------------------------------------------
// Converts a GUID to a printable string of format 4-2-2-2-6
// The returned string needs to be freed outside.
// - guid: An array of 16 bytes containing the guid.
static char * guid_to_string( uint8_t * guid );

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
  uint8_t  * items,
  uint64_t   count,
  size_t     size,
  FILE     * stream
)
{
  g_assert( stream );
  uint64_t len;
  len = fread( (void*)items, size, count, stream );
  if( len != count )
    g_error( "(%s:%d:%s): Could only read %li out of %li items.",
             __FILE__, __LINE__, __func__, len, count );
  if( G_BYTE_ORDER == G_BIG_ENDIAN ) do_byte_swap( items, len, size );
  return true;
}

//============================================================================
//   READ CZI
//============================================================================


bool czi_find_sources( const char * filename, struct _czi * czi, GError ** err )
{
  g_assert( filename );
  g_assert( czi );
  struct _czi_source * source;

  // Add master file
  g_ptr_array_add( czi->sources, g_slice_alloc0( sizeof(struct _czi_source) ) );
  source = (struct _czi_source *) g_ptr_array_index( czi->sources, 0 );
  source->filename = (char*) g_strdup( filename );
  source->begin = 0;
  source->size = 0;
  source->stream = _openslide_fopen( filename, "wb", err );
  if( !source->stream ) return false;

  // Look for eventual part files
  int32_t i = 1;
  char * base = g_strndup( filename, strlen(filename) - strlen(".czi") );
  char * partname = g_strdup_printf( "%s (%d).czi", base, i );
  while( g_file_test( partname, G_FILE_TEST_EXISTS ) )
  {
    g_debug( "Found part file %s", partname );
    g_ptr_array_add( czi->sources, g_slice_alloc0( sizeof(struct _czi_source) ) );
    source = (struct _czi_source*) g_ptr_array_index( czi->sources, i );
    source->filename = partname;
    source->begin = 0;
    source->size = 0;
    source->stream = _openslide_fopen( partname, "wb", err );
    if( !source->stream ) {
      g_free( base );
      return false;
    }
  }
  g_free( partname );
  g_free( base );

  return true;
}

bool czi_decode_one_stream( struct _czi_source * source, struct _czi * czi, GError ** err )
{
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
  bool check_position = source->size > 0;
  struct _czi_segment_header * header = (struct _czi_segment_header*) g_malloc0( sizeof(struct _czi_segment_header) );
  while( !feof( source->stream ) && (!check_position || position < position_max ) )
  {
    if( !czi_read_next_segment_header( source, header, err ) ) {
      // we assume it is because there are no segments left
      g_free( header );
      g_error_free( *err );
      g_free( err );
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
  struct _czi       * czi   G_GNUC_UNUSED,
  struct _czi_tile  * tile  G_GNUC_UNUSED,
  int32_t             ss_x  G_GNUC_UNUSED,
  int32_t             ss_y  G_GNUC_UNUSED,
  GError           ** err   G_GNUC_UNUSED
)
{
  // TODO
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
  g_assert( stream );
  int64_t pos = ftello( stream );

  char magic_string[16];
  if( !read_items( (uint8_t*)magic_string, 16, 1, stream ) ) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to read in stream" );
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
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Failed to initiate _czi structure" );
    return NULL;
  }

  return czi;
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

struct _czi_tile * czi_new_tile( GError ** err G_GNUC_UNUSED )
{
  // TODO
  return NULL;
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
    if( ptr->tiles )       g_hash_table_destroy( ptr->tiles );
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

void czi_free_tile( struct _czi_tile * ptr G_GNUC_UNUSED )
{
  // TODO
  return;
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

  FILE * stream = stream;
  int32_t entry_count;
  int32_t ss_x;
  int32_t ss_y;
  struct _czi_tile * new_tile = NULL;
  struct _czi_dimension * dim;

  read_items( (uint8_t*)&entry_count, 1, sizeof(entry_count), stream );
  fseeko( stream, 124, SEEK_CUR );                       // 124 bytes reserved

  for( int32_t i=0; i<entry_count; ++i )
  {
    new_tile = czi_new_tile( err );
    if( !new_tile ) return false;
    if( !czi_read_tile( source, new_tile, err ) ) {
      czi_free_tile( new_tile );
      return false;
    }
    if( !g_hash_table_contains( new_tile->dimensions, "X" ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Tile without X dimension." );
      czi_free_tile( new_tile );
      return false;
    }
    dim = g_hash_table_lookup( new_tile->dimensions, "X" );
    ss_x = dim->size / dim->stored_size;
    if( !g_hash_table_contains( new_tile->dimensions, "Y" ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "Tile without X dimension." );
      czi_free_tile( new_tile );
      return false;
    }
    dim = g_hash_table_lookup( new_tile->dimensions, "Y" );
    ss_y = dim->size / dim->stored_size;
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
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  file_header->source = source;
  FILE * stream = source->stream;
  int32_t update_pending;

  read_items( (uint8_t*)&(file_header->major),              1, sizeof(file_header->major), stream );
  read_items( (uint8_t*)&(file_header->minor),              1, sizeof(file_header->minor), stream );
  fseeko( stream, 8, SEEK_CUR );                             // 2 int reserved
  read_items( (uint8_t*)file_header->primary_file_guid,    16, sizeof(file_header->primary_file_guid[0]), stream );
  read_items( (uint8_t*)file_header->file_guid,            16, sizeof(file_header->file_guid[0]), stream );
  read_items( (uint8_t*)&(file_header->file_part),          1, sizeof(file_header->file_part), stream );
  read_items( (uint8_t*)&(file_header->directory_position), 1, sizeof(file_header->directory_position), stream );
  read_items( (uint8_t*)&(file_header->metadata_position),  1, sizeof(file_header->metadata_position), stream );
  read_items( (uint8_t*)&(update_pending),                  1, sizeof(update_pending), stream );
  file_header->update_pending = (bool) update_pending;
  read_items( (uint8_t*)&(file_header->attdir_position),    1, sizeof(file_header->attdir_position), stream );

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
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  metadata->source = source;
  FILE * stream = source->stream;

  read_items( (uint8_t*)&(metadata->xml_size),        1, sizeof(metadata->xml_size), stream );
  read_items( (uint8_t*)&(metadata->attachment_size), 1, sizeof(metadata->attachment_size), stream );
  fseeko( stream, 248, SEEK_CUR );                           // end of segment
  fseeko( stream, metadata->xml_size, SEEK_CUR );            // skip xml block
  fseeko( stream, metadata->attachment_size, SEEK_CUR );     // skip att block
  return true;
}

bool czi_read_tile(
  struct _czi_source * source G_GNUC_UNUSED,
  struct _czi_tile * tile     G_GNUC_UNUSED,
  GError ** err               G_GNUC_UNUSED
)
{
  // TODO
  return true;
}

bool czi_read_dimension(
  struct _czi_source * source       G_GNUC_UNUSED,
  struct _czi_dimension * dimension G_GNUC_UNUSED,
  GError ** err                     G_GNUC_UNUSED
)
{
  // TODO
  return true;
}

//============================================================================
//   TO BE SORTED
//============================================================================

#if 0

// TODO error handling
bool readDirectoryEntryDV(
  FILE                    * stream,
  czi_directory_entry_dv  * dirdv,
  GError                 ** err
)
{
  g_assert( stream );
  g_assert( dirdv );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)(dirdv->schema_type), 2, sizeof(dirdv->schema_type[0]), stream );
  readItems( (uint8_t*)&(dirdv->pixel_type), 1, sizeof(dirdv->pixel_type), stream );
  readItems( (uint8_t*)&(dirdv->file_position), 1, sizeof(dirdv->file_position), stream );
  readItems( (uint8_t*)&(dirdv->file_part), 1, sizeof(dirdv->file_part), stream );
  readItems( (uint8_t*)&(dirdv->compression), 1, sizeof(dirdv->compression), stream );
  readItems( (uint8_t*)&(dirdv->pyramid_type), 1, sizeof(dirdv->pyramid_type), stream );
  fseeko( stream, 5, SEEK_CUR );
  readItems( (uint8_t*)&(dirdv->dimension_count), 1, sizeof(dirdv->dimension_count), stream );

  dirdv->dimension_entries = (czi_dimension_entry_dv**) g_try_malloc0_n( dirdv->dimension_count, sizeof(czi_dimension_entry_dv*) );
  if( dirdv->dimension_entries == NULL )
  {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "(%s:%d:%s): Failed to allocate %i x %li bytes.",
                 __FILE__, __LINE__, __func__,
                 dirdv->dimension_count, sizeof(czi_dimension_entry_dv*) );
    return false;
  }
  for( int32_t i=0; i<dirdv->dimension_count; ++i )
  {
    dirdv->dimension_entries[i] = (czi_dimension_entry_dv*) g_try_malloc0( sizeof(czi_dimension_entry_dv) );
    if( dirdv->dimension_entries[i] == NULL )
    {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to allocate %li bytes.",
                   __FILE__, __LINE__, __func__, sizeof(czi_dimension_entry_dv) );
      return false;
    }
    if( !readDimensionEntryDV( stream, dirdv->dimension_entries[i], err ) )
    {
      g_assert (err == NULL || *err != NULL);
      return false;
    }
  }
  return true;
}

// TODO error handling
bool readDimensionEntryDV(
  FILE                    * stream,
  czi_dimension_entry_dv  * dimdv,
  GError                ** err
)
{
  g_assert( stream );
  g_assert( dimdv );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)(dimdv->dimension), 4, sizeof(dimdv->dimension[0]), stream );
  readItems( (uint8_t*)&(dimdv->start), 1, sizeof(dimdv->start), stream );
  readItems( (uint8_t*)&(dimdv->size), 1, sizeof(dimdv->size), stream );
  readItems( (uint8_t*)&(dimdv->start_coordinate), 1, sizeof(dimdv->start_coordinate), stream );
  readItems( (uint8_t*)&(dimdv->stored_size), 1, sizeof(dimdv->stored_size), stream );
  return true;
}


// ===========================================================================
//    OTHER
// ===========================================================================

czi_image_descriptor * newImageDescriptor(
  uint8_t pyramid_type,
  int32_t subsampling_x,
  int32_t subsampling_y
)
{
  czi_image_descriptor * newEntry = (czi_image_descriptor*) calloc( 1, sizeof(czi_image_descriptor) );
  newEntry->pyramid_type = pyramid_type;
  newEntry->subsampling_x = subsampling_x;
  newEntry->subsampling_y = subsampling_y;
  for( int8_t i=0; i<12; ++i ) {
    newEntry->content[i][2] = (int32_t)INT_MAX;
    newEntry->content[i][3] = (int32_t)INT_MIN;
  }
  newEntry->entry_list = NULL;
  newEntry->entry_last = NULL;

  return newEntry;
}

static bool updateImageDescriptor(
  czi_image_descriptor    * imdesc,
  czi_directory_entry_dv  * direntry
)
{
  // add directory
  czi_list_of_directory_entry_dv * tmp = (czi_list_of_directory_entry_dv*) calloc( 1, sizeof(czi_list_of_directory_entry_dv) );
  tmp->entry = direntry;
  tmp->previous = imdesc->entry_last;
  tmp->next = NULL;
  if( imdesc->entry_list ) imdesc->entry_last->next = tmp;
  else                    imdesc->entry_list = tmp;
  imdesc->entry_last = tmp;
  ++(imdesc->entry_count);

  // update computed values
  czi_dimension_entry_dv * dimentry;
  int8_t i = -1;
  for( int32_t j=0; j<direntry->dimension_count; ++j )
  {
    dimentry = direntry->dimension_entries[j];
    switch( dimentry->dimension[0] )
    {
      case 'X':
        i = 0;
        break;
      case 'Y':
        i = 1;
        break;
      case 'C':
        i = 2;
        break;
      case 'Z':
        i = 3;
        break;
      case 'T':
        i = 4;
        break;
      case 'R':
        i = 5;
        break;
      case 'S':
        i = 6;
        break;
      case 'I':
        i = 7;
        break;
      case 'B':
        i = 8;
        break;
      case 'M':
        i = 9;
        break;
      case 'H':
        i = 10;
        break;
      case 'V':
        i = 11;
        break;
      default:
        printf( "computeDimensions: Unknown dimension name %s.", dimentry->dimension );
        break;
    }
    if( i >= 0 )
    {
      int32_t ss = dimentry->size / dimentry->stored_size;
      imdesc->content[i][1] = dimentry->stored_size;
      if( (dimentry->start/ss) < imdesc->content[i][2] )
        imdesc->content[i][2] = dimentry->start / ss;
      if( ((dimentry->start/ss) + dimentry->stored_size) > imdesc->content[i][3] )
        imdesc->content[i][3] = ( dimentry->start / ss ) + dimentry->stored_size;
    }
  }

  return true;
}

czi_image_descriptor * findPyramid(
  czi_list_of_image_descriptor *  listimdesc,
  uint8_t                         pyramid_type,
  int32_t                         subsampling_x,
  int32_t                         subsampling_y
)
{
  czi_list_of_image_descriptor * current, * previous, * newnode;
  czi_image_descriptor         * imdesc;

  // look for existing pyramid image descriptor
  current = listimdesc;
  while( current )
  {
    imdesc = current->entry;
    if( imdesc->pyramid_type == pyramid_type   &&
        imdesc->subsampling_x == subsampling_x &&
        imdesc->subsampling_y == subsampling_y )
    {
      return imdesc;
    }
    current = current->next;
  }

  // create and place new pyramid image descriptor
  imdesc = newImageDescriptor( pyramid_type, subsampling_x, subsampling_y );
  newnode = (czi_list_of_image_descriptor*) g_try_malloc0( sizeof(czi_list_of_image_descriptor) );
  newnode->entry = imdesc;
  current = listimdesc;
  previous = NULL;
  while( current )
  {
    if( current->entry->subsampling_x > subsampling_x )
    {
      // we insert the node here
      // previous cannot be null since the first node is pyr 0
      newnode->next = current;
      newnode->previous = previous;
      current->previous = newnode;
      previous->next = newnode;
      return imdesc;
    }
    previous = current;
    current = current->next;
  }
  newnode->next = NULL;
  newnode->previous = previous;
  previous->next = newnode;
  return imdesc;
}

bool findSubsampling(
  czi_directory_entry_dv  * direntry,
  int32_t                 * ssX,
  int32_t                 * ssY
)
{
  czi_dimension_entry_dv * dimentry;
  for( int32_t i=0; i<direntry->dimension_count; ++i )
  {
    dimentry = direntry->dimension_entries[i];
    if( dimentry->dimension[0] == 'X' )
      *ssX = dimentry->size / dimentry->stored_size;
    if( dimentry->dimension[0] == 'Y' )
      *ssY = dimentry->size / dimentry->stored_size;
  }
  return true;
}

bool computeDimensions(
  czi_subblock_directory_segment  * dirsegment,
  czi_list_of_image_descriptor    * listimdesc,
  int32_t                           maxblocks
)
{
  g_debug( "(%s:%d:%s): compute dimensions", __FILE__, __LINE__,__func__ );
  assert( dirsegment );

  czi_directory_entry_dv * direntry;
  czi_image_descriptor   * imdesc;

  // create pyr 0
  listimdesc->entry = newImageDescriptor( 0, 1, 1 );
  listimdesc->next = NULL;
  listimdesc->previous = NULL;

  int32_t imax;
  if( maxblocks >= 0 )
    imax = maxblocks;
  else
    imax = dirsegment->entry_count;

  int32_t ssx, ssy;
  // read each directory entry
  for( int32_t i=0; i<imax; ++i )
  {
    printf( "%i / %i \r", i+1, imax );
    direntry = dirsegment->entry[i];
    findSubsampling( direntry, &ssx, &ssy );
    imdesc = findPyramid( listimdesc, direntry->pyramid_type, ssx, ssy );
    updateImageDescriptor( imdesc, direntry );
  }
  printf("\n");

  // compute final sizes
  czi_list_of_image_descriptor * current = listimdesc;
  while( current )
  {
    imdesc = current->entry;
    for( int8_t j=0; j<12; ++j )
      imdesc->content[j][0] = imdesc->content[j][3] - imdesc->content[j][2];
    current = current->next;
  }
  return true;
}
#endif

// print

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

  return str;
}

//============================================================================
//
//                            API DEFINITION
//
//============================================================================

//--- check ------------------------------------------------------------------
bool _openslide_czi_is_zisraw( const char * filename, GError ** err )
{
  g_assert( filename );

  FILE * stream = _openslide_fopen( filename, "rb", err );
  if( !stream ) return false;

  if( czi_is_zisraw( stream, err ) )
    return true;
  else
    return false;
}

//--- decode -----------------------------------------------------------------
_openslide_czi * _openslide_czi_decode( const char * filename, GError ** err )
{
  g_assert( filename );

  _openslide_czi * czi = czi_new( err );

  // TODO: look for multiple files
  // While not done: register one file
  if( !czi_find_sources( filename, czi, err ) )
    return NULL;

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

uint8_t * _openslide_czi_load_tile( _openslide_czi * czi, char * guid, GError **err )
{
  // TODO
  return NULL;
}

void _openslide_czi_destroy_tile( _openslide_czi * czi, char * guid, GError **err )
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