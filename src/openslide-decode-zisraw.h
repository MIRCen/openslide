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
typedef struct SegmentHeader {
  char    Id[16];
  int64_t AllocatedSize;
  int64_t UsedSize;
} SegmentHeader;

// ===========================================================================
//    ZISRAWFILE 
// ===========================================================================
typedef struct FileHeader {
  int32_t Major;
  int32_t Minor;
  // int32_t Reserved1;
  // int32_t Reserved2;
  uint8_t PrimaryFileGuid[16];
  uint8_t FileGuid[16];
  int32_t FilePart;
  int64_t DirectoryPosition;
  int64_t MetadataPosition;
  int32_t UpdatePending;
  int64_t AttachmentDirectoryPosition;
} FileHeader;

// ===========================================================================
//    ZISRAWMETADATA 
// ===========================================================================
typedef struct MetadataSegment {
  int32_t XmlSize;
  int32_t AttachmentSize;
  // uint8_t Spare[248];
  uint8_t * xmlBuf;
  xmlDoc  * Xml;
  // attachment -> not used
} MetadataSegment;

// ===========================================================================
//    ZISRAWSUBBLOCK 
// ===========================================================================
typedef struct DimensionEntryDV1 {
  char    Dimension[4];
  int32_t Start;
  int32_t Size;
  float   StartCoordinate;
  int32_t StoredSize;
} DimensionEntryDV;

typedef struct DirectoryEntryDV {
  char               SchemaType[2];
  int32_t            PixelType;
  int64_t            FilePosition;
  int32_t            FilePart;
  int32_t            Compression;
  uint8_t            PyramidType;
  // uint8t_            Reserved1;
  // uint8_t            Reserved2[4];
  int32_t            DimensionCount;
  DimensionEntryDV **DimensionEntries;
} DirectoryEntryDV;

typedef struct SubBlockSegment {
  int32_t             MetadataSize;
  int32_t             AttachmentSize;
  int64_t             DataSize;
  DirectoryEntryDV  **DirectoryEntry;
  // fill to 256
  // xmlDoc * XmlDoc;
} SubBlockSegment;

// ===========================================================================
//    ZISRAWDIRECTORY
// ===========================================================================
typedef struct SubBlockDirectorySegment {
  int32_t EntryCount;
  // uint8_t Reserved;
  DirectoryEntryDV **Entry;
} SubBlockDirectorySegment;

// ===========================================================================
//    ZISRAWATTACH
// ===========================================================================
typedef struct AttachmentEntryA1 {
  char    SchemaType[2];
  // char    Reserved[10];
  int64_t FilePosition;
  int32_t FilePart;
  uint8_t ContentGuid[16];
  char    ContentFileType[8];
  char    Name[80];
} AttachmentEntryA1;

typedef struct AttachmentSegment {
  int32_t DataSize;
  uint8_t Reserved1[12];
  AttachmentEntryA1 * AttachmentEntry;
  // fill to 256
} AttachmentSegment;

// attachments types

typedef struct TimeStampSegment {
  int32_t Size;
  int32_t NumberTimeStamps;
  double * TimeStamps;
} TimeStampSegment;

typedef struct FocusPositions {
  int32_t Size;
  int32_t NumberPositions;
  double * Positions;
} FocusPositions;

typedef struct EventListEntry {
  int32_t Size;
  double Time;
  int32_t EventType;
  int32_t DescriptionSize;
  char * Description;
} EventListEntry;

typedef struct EventListSegment {
  int32_t Size;
  int32_t NumberEvents;
  EventListEntry * Events;
} EnventListSegment;

typedef struct ComponentEntry {
  int32_t Size;
  int32_t ComponentType;
  int32_t NumberIntensities;
  int16_t * Intensity;
} ComponentEntry;

typedef struct LookupTableEntry {
  int32_t Size;
  char Identifier[80];
  int32_t NumberComponents;
  ComponentEntry * Components;
} LookupTableEntry;

typedef struct LookupTableSegment {
  int32_t Size;
  int32_t NumberLookupTables;
  LookupTableEntry * LookupTables;
} LookupTableSegment;

// ===========================================================================
//    ZISRAWATTDIR
// ===========================================================================

typedef struct AttachmentDirectorySegment {
  int32_t EntryCount;
  // fill to 256;
  AttachmentEntryA1 * Entry;
} AttachmentDirectorySegment;

// ===========================================================================
//    OTHER
// ===========================================================================

typedef struct ListOf_DirectoryEntryDV ListOf_DirectoryEntryDV;
struct ListOf_DirectoryEntryDV {
  DirectoryEntryDV * entry;
  ListOf_DirectoryEntryDV * previous;
  ListOf_DirectoryEntryDV * next;
};

typedef struct ImageDescriptor {
  // wich image is described
  uint8_t pyramidType;
  int32_t subsamplingX;
  int32_t subsamplingY;

  // list of entries relted to this image
  int32_t entryCount;
  ListOf_DirectoryEntryDV * entryList;
  ListOf_DirectoryEntryDV * entryLast;

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
} ImageDescriptor;

typedef struct ListOf_ImageDescriptor ListOf_ImageDescriptor;
struct ListOf_ImageDescriptor {
  ImageDescriptor * entry;
  ListOf_ImageDescriptor * previous;
  ListOf_ImageDescriptor * next;
};

// ===========================================================================
//    FUNCTIONS
// ===========================================================================

// navigation
OPENSLIDE_PUBLIC() bool readNextSegmentHeader( FILE * stream, SegmentHeader * segmentheader, GError ** err );
OPENSLIDE_PUBLIC() bool readNextSegmentHeaderWithId( FILE * stream, SegmentHeader * segmentheader, const char * id, GError ** err );
OPENSLIDE_PUBLIC() bool skipSegment( FILE * stream, SegmentHeader * segmentheader, GError ** err );
// compute
OPENSLIDE_PUBLIC() bool computeDimensions( SubBlockDirectorySegment * dirsegment, ListOf_ImageDescriptor * listimdesc, int maxblocks );
// read
OPENSLIDE_PUBLIC() bool readFileHeader( FILE * stream, FileHeader * fileheader, GError ** err );
OPENSLIDE_PUBLIC() bool readSubBlockDirectorySegment( FILE * stream, SubBlockDirectorySegment * dirsegment, GError ** err );
OPENSLIDE_PUBLIC() bool readDirectoryEntryDV( FILE * stream, DirectoryEntryDV * dirdv, GError ** err );
OPENSLIDE_PUBLIC() bool readDimensionEntryDV( FILE * stream, DimensionEntryDV * dimdv, GError ** err );
OPENSLIDE_PUBLIC() bool readMetadataSegment( FILE * stream, MetadataSegment * metaseg, GError ** err );
// free
OPENSLIDE_PUBLIC() void freeSegmentHeader( SegmentHeader * segmentheader );
OPENSLIDE_PUBLIC() void freeFileHeader( FileHeader * fileheader );
OPENSLIDE_PUBLIC() void freeSubBlockDirectorySegment( SubBlockDirectorySegment * dirsegment );
OPENSLIDE_PUBLIC() void freeDirectoryEntryDV( DirectoryEntryDV * dirdv );
OPENSLIDE_PUBLIC() void freeDimensionEntryDV( DimensionEntryDV * dimdv );
OPENSLIDE_PUBLIC() void freeImageDescriptor( ImageDescriptor * imdesc );
OPENSLIDE_PUBLIC() void freeListOf_DirectoryEntryDV( ListOf_DirectoryEntryDV * list );
OPENSLIDE_PUBLIC() void freeListOf_ImageDescriptor( ListOf_ImageDescriptor * list );
OPENSLIDE_PUBLIC() void freeMetadataSegment( MetadataSegment * metaseg );
// print
OPENSLIDE_PUBLIC() void printFileHeader( FileHeader * fileheader );
OPENSLIDE_PUBLIC() void printSubBlockDirectorySegment( SubBlockDirectorySegment * dirsegment, int maxblocks );
OPENSLIDE_PUBLIC() void printDirectoryEntryDV( DirectoryEntryDV * dirdv );
OPENSLIDE_PUBLIC() void printDimensionEntryDV( DimensionEntryDV * dimdv );
OPENSLIDE_PUBLIC() void printPyramids( ListOf_ImageDescriptor * list );
OPENSLIDE_PUBLIC() void printDimensions( ImageDescriptor * imdesc );
// new
OPENSLIDE_PUBLIC() ImageDescriptor * newImageDescriptor( uint8_t pyramidType, int32_t subsamplingX, int32_t subsamplingY );
// other
OPENSLIDE_PUBLIC() ImageDescriptor * findPyramid(
  ListOf_ImageDescriptor * listimdesc,
  uint8_t pyramidType,
  int32_t subsamplingX,
  int32_t subsamplingY
);
OPENSLIDE_PUBLIC() bool findSubsampling(
  DirectoryEntryDV  * direntry,
  int32_t           * ssX,
  int32_t           * ssY
);

#endif