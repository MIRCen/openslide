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

//--- openslide --------------------------------------------------------------
#include "openslide-features.h"    // for OPENSLIDE_PUBLIC(). Must be removed when stable.
//--- std --------------------------------------------------------------------
#include <stdio.h>                                               // FILE *
#include <glib.h>                                                // GError
#include <stdint.h>                                              // intX_t
#include <stdbool.h>                                             // bool
#include <libxml/tree.h>                                         // XmlDoc
//----------------------------------------------------------------------------

// ===========================================================================
//    DEFINE 
// ===========================================================================
// inspired by BioFormats
// Should I keep all these defines here ?

// General
#define CZI_ALIGNMENT   32
#define CZI_HEADER_SIZE 32

// Segment Ids
#define CZI_FILE       "ZISRAWFILE"
#define CZI_DIRECTORY  "ZISRAWDIRECTORY"
#define CZI_SUBBLOCK   "ZISRAWSUBBLOCK"
#define CZI_METADATA   "ZISRAWMETADATA"
#define CZI_ATTACH     "ZISRAWATTACH"
#define CZI_ATTDIR     "ZISRAWATTDIR"
#define CZI_DELETED    "DELETED"

// PixelType
#define CZI_GRAY_8                 0
#define CZI_GRAY_16                1
#define CZI_GRAY_32_FLOAT          2
#define CZI_BGR_24                 3
#define CZI_BGR_48                 4
#define CZI_BGR_96_FLOAT           8
#define CZI_BGRA_32                9
#define CZI_GRAY_64_COMPLEX_FLOAT  10
#define CZI_BGR_192_COMPLEX_FLOAT  11
#define CZI_GRAY_32                12
#define CZI_GRAY_64                13

// Compression
#define CZI_UNCOMPRESSED  0
#define CZI_JPEG          1
#define CZI_LZW           2
#define CZI_JPEGXR        4

// Pyramid
#define CZI_PYRAMID_NONE    0
#define CZI_PYRAMID_SINGLE  1
#define CZI_PYRAMID_MULTI   2

// ===========================================================================
//    SEGMENT HEADER 
// ===========================================================================
typedef struct czi_segment_header {
  char    id[16];
  int64_t allocated_size;
  int64_t used_size;
} czi_segment_header;

// ===========================================================================
//    ZISRAWFILE 
// ===========================================================================
typedef struct czi_file_header {
  int32_t major;
  int32_t minor;
  // int32_t reserved_1;
  // int32_t reserved_2;
  uint8_t primary_file_guid[16];
  uint8_t file_guid[16];
  int32_t file_part;
  int64_t directory_position;
  int64_t metadata_position;
  int32_t update_pending;
  int64_t attachment_directory_position;
} czi_file_header;

// ===========================================================================
//    ZISRAWMETADATA 
// ===========================================================================
typedef struct czi_metadata_segment {
  int32_t xml_size;
  int32_t attachment_size;
  // uint8_t Spare[248];
  uint8_t * xml_buf;
  xmlDoc  * xml;
  // attachment -> not used
} czi_metadata_segment;

// ===========================================================================
//    ZISRAWSUBBLOCK 
// ===========================================================================
typedef struct czi_dimension_entry_dv {
  char    dimension[4];
  int32_t start;
  int32_t size;
  float   start_coordinate;
  int32_t stored_size;
} czi_dimension_entry_dv;

typedef struct czi_directory_entry_dv {
  char                schema_type[2];
  int32_t             pixel_type;
  int64_t             file_position;
  int32_t             file_part;
  int32_t             compression;
  uint8_t             pyramid_type;
  // uint8t_             reserved_1;
  // uint8_t             reserved 2[4];
  int32_t             dimension_count;
  czi_dimension_entry_dv ** dimension_entries;
} czi_directory_entry_dv;

typedef struct czi_subblock_segment {
  int32_t              metadata_size;
  int32_t              attachment_size;
  int64_t              data_size;
  czi_directory_entry_dv  ** directory_entry;
  // fill to 256
  // xmlDoc * xml;
} czi_subblock_segment;

// ===========================================================================
//    ZISRAWDIRECTORY
// ===========================================================================
typedef struct czi_subblock_directory_segment {
  int32_t entry_count;
  // uint8_t reserved;
  czi_directory_entry_dv ** entry;
} czi_subblock_directory_segment;

// ===========================================================================
//    ZISRAWATTACH
// ===========================================================================
typedef struct czi_attachment_entry_a1 {
  char    schema_type[2];
  // char    Reserved[10];
  int64_t file_position;
  int32_t file_part;
  uint8_t content_guid[16];
  char    content_file_type[8];
  char    name[80];
} czi_attachment_entry_a1;

typedef struct czi_attachment_segment {
  int32_t data_size;
  uint8_t reserved_1[12];
  czi_attachment_entry_a1 * attachment_entry;
  // fill to 256
} czi_attachment_segment;

// attachments types

typedef struct czi_timestamp_segment {
  int32_t size;
  int32_t number_timestamps;
  double * timestamps;
} czi_timestamp_segment;

typedef struct czi_focus_positions {
  int32_t size;
  int32_t number_positions;
  double * positions;
} czi_focus_positions;

typedef struct czi_event_list_entry {
  int32_t   size;
  double    time;
  int32_t   event_type;
  int32_t   description_size;
  char    * description;
} czi_event_list_entry;

typedef struct czi_event_list_segment {
  int32_t size;
  int32_t number_events;
  czi_event_list_entry * events;
} czi_event_list_segment;

typedef struct czi_component_entry {
  int32_t   size;
  int32_t   component_type;
  int32_t   number_intensities;
  int16_t * intensity;
} czi_component_entry;

typedef struct czi_lookup_table_entry {
  int32_t               size;
  char                  identifier[80];
  int32_t               number_components;
  czi_component_entry * components;
} czi_lookup_table_entry;

typedef struct czi_lookup_table_segment {
  int32_t                  size;
  int32_t                  number_lookup_tables;
  czi_lookup_table_entry * lookup_tables;
} czi_lookup_table_segment;

// ===========================================================================
//    ZISRAWATTDIR
// ===========================================================================

typedef struct czi_attachment_directory_segment {
  int32_t entry_count;
  // fill to 256;
  czi_attachment_entry_a1 * entry;
} czi_attachment_directory_segment;

// ===========================================================================
//    OTHER
// ===========================================================================

typedef struct czi_list_of_directory_entry_dv czi_list_of_directory_entry_dv;
struct czi_list_of_directory_entry_dv {
  czi_directory_entry_dv          * entry;
  czi_list_of_directory_entry_dv * previous;
  czi_list_of_directory_entry_dv * next;
};

typedef struct czi_image_descriptor {
  // wich image is described
  uint8_t pyramid_type;
  int32_t subsampling_x;
  int32_t subsampling_y;

  // list of entries related to this image
  int32_t                          entry_count;
  czi_list_of_directory_entry_dv * entry_list;
  czi_list_of_directory_entry_dv * entry_last;

  // Computed dimensions
  // For each of 12 possible dimensions:
  //  - X
  //  - Y
  //  - C (Channel)
  //  - Z
  //  - T (Time point)
  //  - R (Rotation)
  //  - S (Scene)
  //  - I (Illumination)
  //  - B (Block)
  //  - M (Mosaic tile index)
  //  - H (Phase index)
  //  - V (View index)
  // We keep 4 values:
  //  - total size
  //  - tile size
  //  - starting index
  //  - max index + 1
  int32_t content[12][4];
} czi_image_descriptor;

typedef struct czi_list_of_image_descriptor czi_list_of_image_descriptor;
struct czi_list_of_image_descriptor {
  czi_image_descriptor         * entry;
  czi_list_of_image_descriptor * previous;
  czi_list_of_image_descriptor * next;
};

// ===========================================================================
//    TYPEDEF POINTERS
// ===========================================================================

typedef czi_segment_header*                czi_segment_header_ptr;
typedef czi_file_header*                   czi_file_header_ptr;
typedef czi_metadata_segment*              czi_metadata_segment_ptr;
typedef czi_dimension_entry_dv*            czi_dimension_entry_dv_ptr;
typedef czi_directory_entry_dv*            czi_directory_entry_dv_ptr;
typedef czi_subblock_segment*              czi_subblock_segment_ptr;
typedef czi_subblock_directory_segment*    czi_subblock_directory_segment_ptr;
typedef czi_attachment_entry_a1*           czi_attachment_entry_a1_ptr;
typedef czi_attachment_segment*            czi_attachment_segment_ptr;
typedef czi_timestamp_segment*             czi_timestamp_segment_ptr;
typedef czi_focus_positions*               czi_focus_positions_ptr;
typedef czi_event_list_entry*              czi_event_list_entry_ptr;
typedef czi_event_list_segment*            czi_event_list_segment_ptr;
typedef czi_component_entry*               czi_component_entry_ptr;
typedef czi_lookup_table_entry*            czi_lookup_table_entry_ptr;
typedef czi_lookup_table_segment*          czi_lookup_table_segment_ptr;
typedef czi_attachment_directory_segment*  czi_attachment_directory_segment_ptr;
typedef czi_image_descriptor*              czi_image_descriptor_ptr;
typedef czi_list_of_directory_entry_dv*    czi_list_of_directory_entry_dv_ptr;
typedef czi_list_of_image_descriptor*      czi_list_of_image_descriptor_ptr;

// ===========================================================================
//    FUNCTIONS
// ===========================================================================

// navigation
OPENSLIDE_PUBLIC() bool readNextSegmentHeader( FILE * stream, czi_segment_header * segmentheader, GError ** err );
OPENSLIDE_PUBLIC() bool readNextSegmentHeaderWithId( FILE * stream, czi_segment_header * segmentheader, const char * id, GError ** err );
OPENSLIDE_PUBLIC() bool skipSegment( FILE * stream, czi_segment_header * segmentheader, GError ** err );
// compute
OPENSLIDE_PUBLIC() bool computeDimensions( czi_subblock_directory_segment * dirsegment, czi_list_of_image_descriptor * listimdesc, int32_t maxblocks );
// read
OPENSLIDE_PUBLIC() bool readFileHeader(               FILE * stream, czi_file_header * fileheader,                GError ** err );
OPENSLIDE_PUBLIC() bool readSubBlockDirectorySegment( FILE * stream, czi_subblock_directory_segment * dirsegment, GError ** err );
OPENSLIDE_PUBLIC() bool readDirectoryEntryDV(         FILE * stream, czi_directory_entry_dv * dirdv,              GError ** err );
OPENSLIDE_PUBLIC() bool readDimensionEntryDV(         FILE * stream, czi_dimension_entry_dv * dimdv,              GError ** err );
OPENSLIDE_PUBLIC() bool readMetadataSegment(          FILE * stream, czi_metadata_segment * metaseg,              GError ** err );
// free
OPENSLIDE_PUBLIC() void freeSegmentHeader(            czi_segment_header * segmentheader );
OPENSLIDE_PUBLIC() void freeFileHeader(               czi_file_header * fileheader );
OPENSLIDE_PUBLIC() void freeSubBlockDirectorySegment( czi_subblock_directory_segment * dirsegment );
OPENSLIDE_PUBLIC() void freeDirectoryEntryDV(         czi_directory_entry_dv * dirdv );
OPENSLIDE_PUBLIC() void freeDimensionEntryDV(         czi_dimension_entry_dv * dimdv );
OPENSLIDE_PUBLIC() void freeImageDescriptor(          czi_image_descriptor * imdesc );
OPENSLIDE_PUBLIC() void freeListOf_DirectoryEntryDV(  czi_list_of_directory_entry_dv * list );
OPENSLIDE_PUBLIC() void freeListOf_ImageDescriptor(   czi_list_of_image_descriptor * list );
OPENSLIDE_PUBLIC() void freeMetadataSegment(          czi_metadata_segment * metaseg );
// print
OPENSLIDE_PUBLIC() void printFileHeader(               czi_file_header * fileheader );
OPENSLIDE_PUBLIC() void printSubBlockDirectorySegment( czi_subblock_directory_segment * dirsegment, int maxblocks );
OPENSLIDE_PUBLIC() void printDirectoryEntryDV(         czi_directory_entry_dv * dirdv );
OPENSLIDE_PUBLIC() void printDimensionEntryDV(         czi_dimension_entry_dv * dimdv );
OPENSLIDE_PUBLIC() void printPyramids(                 czi_list_of_image_descriptor * list );
OPENSLIDE_PUBLIC() void printDimensions(               czi_image_descriptor * imdesc );
// new
OPENSLIDE_PUBLIC() czi_image_descriptor * newImageDescriptor( uint8_t pyramid_type, int32_t subsampling_x, int32_t subsampling_y );
// other
OPENSLIDE_PUBLIC() czi_image_descriptor * findPyramid(
  czi_list_of_image_descriptor *  listimdesc,
  uint8_t                         pyramid_type,
  int32_t                         subsampling_x,
  int32_t                         subsampling_y
);
OPENSLIDE_PUBLIC() bool findSubsampling(
  czi_directory_entry_dv  * direntry,
  int32_t                 * ssX,
  int32_t                 * ssY
);

#endif