/*
 * ZEISS (czi) zisraw support
 * Yaël
 */

//--- openslide --------------------------------------------------------------
// I guess it's better ton include config in sources
#include <config.h>
#include "openslide-decode-zisraw.h"
// for now xml decoding is done here. Maybe should we move it to decode-zeiss 
// instead so that drivers are independant.
#include "openslide-decode-xml.h"
 // for _openslide_fopen and other utils.
#include "openslide-private.h"
//--- std --------------------------------------------------------------------
#include <assert.h>  // use glib instead ?
#include <string.h>
#include <stdlib.h>  // alloc: use glib instead ?
#include <stdio.h>
#include <sys/types.h>  // what for ?
#include <limits.h>  // fmin/fmax : should do otherwise since I comp integers.
#include <errno.h>    // errno
//--- preprocessing stuff ----------------------------------------------------
#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x100)
//----------------------------------------------------------------------------

//============================================================================
//   UTILS
//============================================================================

// Apply byte swapping on an array of items.
static bool doByteSwap(
  uint8_t   * items,    // Pointer to memory block to process
  uint64_t    count,    // Number of items in the array.
  size_t      size      // Size of one item
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

// Reads a series of items from file and eventually applies byte swapping.
// It needs macro IS_BIG_ENDIAN to be defined.
// Byte swapping is applied when IS_BIG_ENDIAN is true, sinces data in a CZI
// file are stored in little endian.
static bool readItems(
  uint8_t  * items,    // Pointer to memory block to process
  uint64_t   count,    // Number of items in the array
  size_t     size,     // Size of one item
  FILE     * stream    // File stream
)
{
  g_assert( stream );
  uint64_t len;
  len = fread( (void*)items, size, count, stream );
  if( IS_BIG_ENDIAN ) doByteSwap( items, len, size );
  if( len != count )
    g_error( "Could only read %li out of %li items.", len, count );
  return true;
}

// Converts a GUID to a printable string of format 4-2-2-2-6
// The returned string needs to be freed outside.
static char * guidToString(
  uint8_t * guid    // An array of 16 bytes containing the guid.
)
{
  char * str = malloc(36);
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
//   FIND SEGMENTS
//============================================================================

bool readNextSegmentHeader(
  FILE           * stream,
  SegmentHeader  * segmentheader,
  GError        ** err
)
{
  g_assert( stream );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  off_t current_pos=-1;
  off_t previous_pos=-1;

  char Id[16];
  while( !feof( stream ) )
  {  
    // 32 bytes alignment
    if( ( current_pos = ftello( stream ) ) == -1 ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "openslide-decode-zisraw.c::readNextSegmentHeader: "
                   "Failed to read position in file stream: %s",
                   g_strerror(errno) );
      return false;
    }
    if( fseeko( stream, current_pos % CZI_ALIGNMENT, SEEK_CUR ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "openslide-decode-zisraw.c::readNextSegmentHeader: "
                   "Failed to seek position CUR+%li in file stream: %s",
                   current_pos % CZI_ALIGNMENT, g_strerror(errno) );
      return false;
    }
    if( current_pos == previous_pos ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "openslide-decode-zisraw.c::readNextSegmentHeader: "
                   "We're not moving in file anymore. "
                   "Better break the loop and go to end of file. "
                   "At %li.", ftello(stream) );
      fseeko( stream, 0, SEEK_END );
      return false;
    }

    // read
    if( fread( (void*)Id, sizeof(Id[0]), sizeof(Id), stream ) != sizeof(Id) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "openslide-decode-zisraw.c::readNextSegmentHeader: "
                   "Failed to read %li items: %s", sizeof(Id),
                   g_strerror(errno) );
      return false;
    }
    if( !strcmp( Id, CZI_FILE )      ||
        !strcmp( Id, CZI_DIRECTORY ) ||
        !strcmp( Id, CZI_SUBBLOCK )  ||
        !strcmp( Id, CZI_METADATA )  ||
        !strcmp( Id, CZI_ATTACH )    ||
        !strcmp( Id, CZI_ATTDIR )    ||
        !strcmp( Id, CZI_DELETED )
      )
    {
      strcpy( segmentheader->Id, Id );
      int64_t len;
      len = fread( (void*)&(segmentheader->AllocatedSize), sizeof(segmentheader->AllocatedSize), 1, stream );
      if( len != 1 )
      {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "openslide-decode-zisraw.c::readNextSegmentHeader: "
                     "Failed to read %li items (only %li): %s",
                     len, sizeof(segmentheader->AllocatedSize),
                     g_strerror(errno) );
        return false;
      }
      len = fread( (void*)&(segmentheader->UsedSize), sizeof(segmentheader->UsedSize), 1, stream );
      if( len != 1 )
      {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "openslide-decode-zisraw.c::readNextSegmentHeader: "
                     "Failed to read %li items (only %li): %s",
                     len, sizeof(segmentheader->UsedSize),
                     g_strerror(errno) );
        return false;
      }
      return true;
    }
    previous_pos = current_pos;
  }

  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "openslide-decode-zisraw.c::readNextSegmentHeader: "
               "No segment left." );
  return false;
}

bool readNextSegmentHeaderWithId(
  FILE           * stream,
  SegmentHeader  * segmentheader,
  const char     * id,
  GError        ** err
)
{
  g_assert( stream );
  g_assert( segmentheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  while( !feof( stream ) )
  {
    if( !readNextSegmentHeader( stream, segmentheader, err ) ) {
      g_assert (err == NULL || *err != NULL);
      return false;
    }
    if( strcmp( id, segmentheader->Id ) )
    {
      return true;
    }
    else
    {
      if( !skipSegment( stream, segmentheader, err ) ) {
        g_assert (err == NULL || *err != NULL);
        return false;
      }
    }
  }

  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "openslide-decode-zisraw.c::readNextSegmentHeaderWithId: "
               "No segment %s found.", id );
  return false;
}

bool skipSegment(
  FILE           * stream,
  SegmentHeader  * segmentheader,
  GError        ** err
)
{
  g_assert( stream );
  g_assert( segmentheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  if( fseeko( stream, segmentheader->AllocatedSize, SEEK_CUR ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "openslide-decode-zisraw.c::skipSegment: "
                   "Failed to seek position CUR+%li in file stream: %s",
                   segmentheader->AllocatedSize, g_strerror(errno) );
      return false;
  }

  return true;
}

//============================================================================
//   READ SEGMENTS
//============================================================================

// TODO error handling
bool readFileHeader(
  FILE        * stream,
  FileHeader  * fileheader,
  GError     ** err
)
{
  g_assert( stream );
  g_assert( fileheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)&(fileheader->Major), 1, sizeof(fileheader->Major), stream );
  readItems( (uint8_t*)&(fileheader->Minor), 1, sizeof(fileheader->Minor), stream );
  fseeko( stream, 8, SEEK_CUR );
  readItems( (uint8_t*)fileheader->PrimaryFileGuid, 16, sizeof(fileheader->PrimaryFileGuid[0]), stream );
  readItems( (uint8_t*)fileheader->FileGuid, 16, sizeof(fileheader->FileGuid[0]), stream );
  readItems( (uint8_t*)&(fileheader->FilePart), 1, sizeof(fileheader->FilePart), stream );
  readItems( (uint8_t*)&(fileheader->DirectoryPosition), 1, sizeof(fileheader->DirectoryPosition), stream );
  readItems( (uint8_t*)&(fileheader->MetadataPosition), 1, sizeof(fileheader->MetadataPosition), stream );
  readItems( (uint8_t*)&(fileheader->UpdatePending), 1, sizeof(fileheader->UpdatePending), stream );
  readItems( (uint8_t*)&(fileheader->AttachmentDirectoryPosition), 1, sizeof(fileheader->AttachmentDirectoryPosition), stream );

  return true;
}

// TODO error handling
bool readSubBlockDirectorySegment(
  FILE                      * stream,
  SubBlockDirectorySegment  * dirsegment,
  GError                   ** err
)
{
  g_assert( stream );
  g_assert( dirsegment );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)&(dirsegment->EntryCount), 1, sizeof(dirsegment->EntryCount), stream );
  fseeko( stream, 124, SEEK_CUR );

  dirsegment->Entry = (DirectoryEntryDV**) calloc( dirsegment->EntryCount, sizeof(DirectoryEntryDV*) );
  for( int32_t i=0; i<dirsegment->EntryCount; ++i )
  {
    dirsegment->Entry[i] = (DirectoryEntryDV*) calloc( 1, sizeof(DirectoryEntryDV) );
    if( !readDirectoryEntryDV( stream, dirsegment->Entry[i], err ) )
    {
      g_assert (err == NULL || *err != NULL);
      return false;
    }
  }
  return true;
}

// TODO error handling
bool readDirectoryEntryDV(
  FILE              * stream,
  DirectoryEntryDV  * dirdv,
  GError           ** err
)
{
  g_assert( stream );
  g_assert( dirdv );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)(dirdv->SchemaType), 2, sizeof(dirdv->SchemaType[0]), stream );
  readItems( (uint8_t*)&(dirdv->PixelType), 1, sizeof(dirdv->PixelType), stream );
  readItems( (uint8_t*)&(dirdv->FilePosition), 1, sizeof(dirdv->FilePosition), stream );
  readItems( (uint8_t*)&(dirdv->FilePart), 1, sizeof(dirdv->FilePart), stream );
  readItems( (uint8_t*)&(dirdv->Compression), 1, sizeof(dirdv->Compression), stream );
  readItems( (uint8_t*)&(dirdv->PyramidType), 1, sizeof(dirdv->PyramidType), stream );
  fseeko( stream, 5, SEEK_CUR );
  readItems( (uint8_t*)&(dirdv->DimensionCount), 1, sizeof(dirdv->DimensionCount), stream );

  dirdv->DimensionEntries = (DimensionEntryDV**) calloc( dirdv->DimensionCount, sizeof(DimensionEntryDV*) );
  for( int32_t i=0; i<dirdv->DimensionCount; ++i )
  {
    dirdv->DimensionEntries[i] = (DimensionEntryDV*) calloc( 1, sizeof(DimensionEntryDV) );
    if( !readDimensionEntryDV( stream, dirdv->DimensionEntries[i], err ) )
    {
      g_assert (err == NULL || *err != NULL);
      return false;
    }
  }
  return true;
}

// TODO error handling
bool readDimensionEntryDV(
  FILE              * stream,
  DimensionEntryDV  * dimdv,
  GError           ** err
)
{
  g_assert( stream );
  g_assert( dimdv );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)(dimdv->Dimension), 4, sizeof(dimdv->Dimension[0]), stream );
  readItems( (uint8_t*)&(dimdv->Start), 1, sizeof(dimdv->Start), stream );
  readItems( (uint8_t*)&(dimdv->Size), 1, sizeof(dimdv->Size), stream );
  readItems( (uint8_t*)&(dimdv->StartCoordinate), 1, sizeof(dimdv->StartCoordinate), stream );
  readItems( (uint8_t*)&(dimdv->StoredSize), 1, sizeof(dimdv->StoredSize), stream );
  return true;
}

// TODO error handling
bool readMetadataSegment(
  FILE             * stream,
  MetadataSegment  * metaseg,
  GError          ** err
)
{
  readItems( (uint8_t*)&(metaseg->XmlSize), 1, sizeof(metaseg->XmlSize), stream );
  readItems( (uint8_t*)&(metaseg->AttachmentSize), 1, sizeof(metaseg->AttachmentSize), stream );
  fseeko( stream, 248, SEEK_CUR );
  metaseg->xmlBuf = (uint8_t*) malloc( metaseg->XmlSize );
  readItems( (uint8_t*)metaseg->xmlBuf, metaseg->XmlSize, sizeof(uint8_t), stream );
  // keep xml decoding here ?
  GError *tmp_err = NULL;
  metaseg->Xml = _openslide_xml_parse( (const char *) metaseg->xmlBuf, &tmp_err );
  if( tmp_err != NULL )
  {
    g_propagate_error( err, tmp_err );
    return false;
  }
  return true;
}

//============================================================================
//   PRINT SEGMENTS
//============================================================================

void printFileHeader( FileHeader * fileheader )
{
  char * PFguid = guidToString( fileheader->PrimaryFileGuid );
  char * Fguid = guidToString( fileheader->FileGuid );
  printf( "+-----------------------------------------------------------+\n" );
  printf( "|                        FileHeader                         |\n" );
  printf( "+-----------------------------------------------------------+\n" );
  printf( "| - Major: \t %li \n", (long int)fileheader->Major );
  printf( "| - Minor: \t %li \n", (long int)fileheader->Minor );
  printf( "| - PrimaryFileGuid: \t %s \n", PFguid );
  printf( "| - FileGuid: \t %s \n", Fguid );
  printf( "| - FilePart: \t %li \n", (long int)fileheader->FilePart );
  printf( "| - DirectoryPosition: \t %li \n", (long int)fileheader->DirectoryPosition );
  printf( "| - MetadataPosition: \t %li \n", (long int)fileheader->MetadataPosition );
  printf( "| - UpdatePending: \t %li \n", (long int)fileheader->UpdatePending );
  printf( "| - AttachmentDirectoryPosition: \t %li \n", (long int)fileheader->AttachmentDirectoryPosition );
  printf( "+-----------------------------------------------------------+\n" );
  free( PFguid );
  free( Fguid );
}

void printSubBlockDirectorySegment( SubBlockDirectorySegment * dirsegment, int maxblocks )
{
  printf( "+-----------------------------------------------------------+\n" );
  printf( "|                 SubBlockDirectorySegment                  |\n" );
  printf( "+-----------------------------------------------------------+\n" );
  printf( "| - EntryCount: \t %li \n", (long int)dirsegment->EntryCount );
  int imax;
  if( maxblocks >= 0 )
    imax = maxblocks;
  else
    imax = dirsegment->EntryCount;
  for( int i=0; i<imax; ++i )
  {
    printDirectoryEntryDV( dirsegment->Entry[i] );
  }
  printf( "+-----------------------------------------------------------+\n" );
}

void printDirectoryEntryDV( DirectoryEntryDV * dirdv )
{
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| |                    DirectoryEntryDV                     |\n" );
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| | - SchemaType: \t %s \n", dirdv->SchemaType );
  printf( "| | - PixelType: \t %li \n", (long int)dirdv->PixelType );
  printf( "| | - FilePosition: \t %li \n", (long int)dirdv->FilePosition );
  printf( "| | - FilePart: \t %li \n", (long int)dirdv->FilePart );
  printf( "| | - Compression: \t %li \n", (long int)dirdv->Compression );
  printf( "| | - PyramidType: \t %li \n", (long int)dirdv->PyramidType );
  printf( "| | - DimensionCount: \t %li \n", (long int)dirdv->DimensionCount );
  for( int i=0; i<dirdv->DimensionCount; ++i )
  {
    printDimensionEntryDV( dirdv->DimensionEntries[i] );
  }
  printf( "| +---------------------------------------------------------+\n" );
}

void printDimensionEntryDV( DimensionEntryDV * dimdv )
{
  printf( "| | +-------------------------------------------------------+\n" );
  printf( "| | |                   DimensionEntryDV                    |\n" );
  printf( "| | +-------------------------------------------------------+\n" );
  printf( "| | | - Dimension: \t %s \n", dimdv->Dimension );
  printf( "| | | - Start: \t %li \n", (long int)dimdv->Start );
  printf( "| | | - Size: \t %li \n", (long int)dimdv->Size );
  printf( "| | | - StartCoordinate: \t %lf \n", dimdv->StartCoordinate );
  printf( "| | | - StoredSize: \t %li \n", (long int)dimdv->StoredSize );
  printf( "| | +-------------------------------------------------------+\n" );
}

void printPyramids( ListOf_ImageDescriptor * list )
{
  printf( "+-----------------------------------------------------------+\n" );
  printf( "|                         Pyramids                          |\n" );
  printf( "+-----------------------------------------------------------+\n" );
  ListOf_ImageDescriptor * current = list;
  while( current )
  {
    printDimensions( current->entry );
    current = current->next;
  }
}

void printDimensions( ImageDescriptor * imdesc )
{
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| |                       Dimensions                        |\n" );
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| | - PyramidType: \t %i\n",    (unsigned char) imdesc->pyramidType );
  printf( "| | - SubSamplingX: \t %li\n",  (long int) imdesc->subsamplingX );
  printf( "| | - SubSamplingY: \t %li\n",  (long int) imdesc->subsamplingY );
  printf( "| | - EntryCount: \t %li\n",    (long int) imdesc->entryCount );
  printf( "| | \n" );
  printf( "| | - SizeX: \t %li\n",          (long int) imdesc->content[0][0] );
  printf( "| | - SizeY: \t %li\n",          (long int) imdesc->content[1][0] );
  printf( "| | - SizeC: \t %li\n",          (long int) imdesc->content[2][0] );
  printf( "| | - SizeZ: \t %li\n",          (long int) imdesc->content[3][0] );
  printf( "| | - SizeT: \t %li\n",          (long int) imdesc->content[4][0] );
  printf( "| | - rotations: \t %li\n",      (long int) imdesc->content[5][0] );
  printf( "| | - scenes: \t %li\n",         (long int) imdesc->content[6][0] );
  printf( "| | - illuminations: \t %li\n",  (long int) imdesc->content[7][0] );
  printf( "| | - blocks: \t %li\n",         (long int) imdesc->content[8][0] );
  printf( "| | - mosaics: \t %li\n",        (long int) imdesc->content[9][0] );
  printf( "| | - phases: \t %li\n",         (long int) imdesc->content[10][0] );
  printf( "| | - views: \t %li\n",          (long int) imdesc->content[11][0] );
  printf( "| | \n" );
  printf( "| | - tileSizeX: \t %li\n",  (long int) imdesc->content[0][1] );
  printf( "| | - tileSizeY: \t %li\n",  (long int) imdesc->content[1][1] );
  printf( "| | - tileSizeC: \t %li\n",  (long int) imdesc->content[2][1] );
  printf( "| | - tileSizeZ: \t %li\n",  (long int) imdesc->content[3][1] );
  printf( "| | - tileSizeT: \t %li\n",  (long int) imdesc->content[4][1] );
  printf( "| | - tileSizeR: \t %li\n",  (long int) imdesc->content[5][1] );
  printf( "| | - tileSizeS: \t %li\n",  (long int) imdesc->content[6][1] );
  printf( "| | - tileSizeI: \t %li\n",  (long int) imdesc->content[7][1] );
  printf( "| | - tileSizeB: \t %li\n",  (long int) imdesc->content[8][1] );
  printf( "| | - tileSizeM: \t %li\n",  (long int) imdesc->content[9][1] );
  printf( "| | - tileSizeH: \t %li\n",  (long int) imdesc->content[10][1] );
  printf( "| | - tileSizeV: \t %li\n",  (long int) imdesc->content[11][1] );
  printf( "| | \n" );
  printf( "| | - startX: \t %li\n",  (long int) imdesc->content[0][2] );
  printf( "| | - startY: \t %li\n",  (long int) imdesc->content[1][2] );
  printf( "| | - startC: \t %li\n",  (long int) imdesc->content[2][2] );
  printf( "| | - startZ: \t %li\n",  (long int) imdesc->content[3][2] );
  printf( "| | - startT: \t %li\n",  (long int) imdesc->content[4][2] );
  printf( "| | - startR: \t %li\n",  (long int) imdesc->content[5][2] );
  printf( "| | - startS: \t %li\n",  (long int) imdesc->content[6][2] );
  printf( "| | - startI: \t %li\n",  (long int) imdesc->content[7][2] );
  printf( "| | - startB: \t %li\n",  (long int) imdesc->content[8][2] );
  printf( "| | - startM: \t %li\n",  (long int) imdesc->content[9][2] );
  printf( "| | - startH: \t %li\n",  (long int) imdesc->content[10][2] );
  printf( "| | - startV: \t %li\n",  (long int) imdesc->content[11][2] );
  printf( "| +---------------------------------------------------------+\n" );
}

//============================================================================
//   FREE
//============================================================================

void freeSegmentHeader( SegmentHeader * segmentheader )
{
  if( segmentheader )
    free( segmentheader );
}

void freeFileHeader( FileHeader * fileheader )
{
  if( fileheader )
    free( fileheader );
}

void freeSubBlockDirectorySegment( SubBlockDirectorySegment * dirsegment )
{
  if( dirsegment )
  {
    if( dirsegment->Entry )
    {
      for( int32_t i=0; i<dirsegment->EntryCount; ++i )
        freeDirectoryEntryDV( dirsegment->Entry[i] );
      free( dirsegment->Entry );
    }
    free( dirsegment );
  }
}

void freeDirectoryEntryDV( DirectoryEntryDV * dirdv )
{
  if( dirdv )
  {
    if( dirdv->DimensionEntries )
    {
      for( int32_t i=0; i<dirdv->DimensionCount; ++i )
        freeDimensionEntryDV( dirdv->DimensionEntries[i] );
      free( dirdv->DimensionEntries );
    }
    free( dirdv );
  }
}

void freeDimensionEntryDV( DimensionEntryDV * dimdv )
{
  if( dimdv )
    free( dimdv );
}

void freeImageDescriptor( ImageDescriptor * imdesc )
{
  if( imdesc ) {
    freeListOf_DirectoryEntryDV( imdesc->entryList );
    free( imdesc );
  }
}

void freeListOf_DirectoryEntryDV( ListOf_DirectoryEntryDV * list )
{
  if( list )
  {
    freeListOf_DirectoryEntryDV( list->next );
    free( list );
  }
}

void freeListOf_ImageDescriptor( ListOf_ImageDescriptor * list )
{
  if( list )
  {
    freeListOf_ImageDescriptor( list->next );
    free( list );
  }
}


void freeMetadataSegment( MetadataSegment * metaseg )
{
  if( metaseg )
  {
    if( metaseg->xmlBuf )
      free( metaseg->xmlBuf );
    if( metaseg->Xml )
      xmlFreeDoc( metaseg->Xml );
    free( metaseg );
  }
}

// ===========================================================================
//    OTHER
// ===========================================================================

ImageDescriptor * newImageDescriptor(
  uint8_t pyramidType,
  int32_t subsamplingX,
  int32_t subsamplingY
)
{
  ImageDescriptor * newEntry = (ImageDescriptor*) calloc( 1, sizeof(ImageDescriptor) );
  newEntry->pyramidType = pyramidType;
  newEntry->subsamplingX = subsamplingX;
  newEntry->subsamplingY = subsamplingY;
  for( int8_t i=0; i<12; ++i ) {
    newEntry->content[i][2] = (int32_t)INT_MAX;
    newEntry->content[i][3] = (int32_t)INT_MIN;
  }
  newEntry->entryList = NULL;
  newEntry->entryLast = NULL;

  return newEntry;
}

static bool updateImageDescriptor(
  ImageDescriptor * imdesc,
  DirectoryEntryDV * direntry
)
{
  // add directory
  ListOf_DirectoryEntryDV * tmp = (ListOf_DirectoryEntryDV*) calloc( 1, sizeof(ListOf_DirectoryEntryDV) );
  tmp->entry = direntry;
  tmp->previous = imdesc->entryLast;
  tmp->next = NULL;
  if( imdesc->entryList ) imdesc->entryLast->next = tmp;
  else                    imdesc->entryList = tmp;
  imdesc->entryLast = tmp;
  ++(imdesc->entryCount);

  // update computed values
  DimensionEntryDV * dimentry;
  int8_t i = -1;
  for( int32_t j=0; j<direntry->DimensionCount; ++j )
  {
    dimentry = direntry->DimensionEntries[j];
    switch( dimentry->Dimension[0] )
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
        printf( "computeDimensions: Unknown dimension name %s.", dimentry->Dimension );
        break;
    }
    if( i >= 0 )
    {
      int32_t ss = dimentry->Size / dimentry->StoredSize;
      imdesc->content[i][1] = dimentry->StoredSize;
      if( (dimentry->Start/ss) < imdesc->content[i][2] )
        imdesc->content[i][2] = dimentry->Start / ss;
      if( ((dimentry->Start/ss) + dimentry->StoredSize) > imdesc->content[i][3] )
        imdesc->content[i][3] = ( dimentry->Start / ss ) + dimentry->StoredSize;
    }
  }

  return true;
}

ImageDescriptor * findPyramid(
  ListOf_ImageDescriptor * listimdesc,
  uint8_t pyramidType,
  int32_t subsamplingX,
  int32_t subsamplingY
)
{
  ListOf_ImageDescriptor * current, * previous, * newnode;
  ImageDescriptor        * imdesc;

  // look for existing pyramid image descriptor
  current = listimdesc;
  while( current )
  {
    imdesc = current->entry;
    if( imdesc->pyramidType == pyramidType   &&
        imdesc->subsamplingX == subsamplingX &&
        imdesc->subsamplingY == subsamplingY )
    {
      return imdesc;
    }
    current = current->next;
  }

  // create and place new pyramid image descriptor
  imdesc = newImageDescriptor( pyramidType, subsamplingX, subsamplingY );
  newnode = (ListOf_ImageDescriptor*) calloc( 1, sizeof(ListOf_ImageDescriptor) );
  newnode->entry = imdesc;
  current = listimdesc;
  previous = NULL;
  while( current )
  {
    if( current->entry->subsamplingX > subsamplingX )
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
  DirectoryEntryDV  * direntry,
  int32_t           * ssX,
  int32_t           * ssY
)
{
  DimensionEntryDV * dimentry;
  for( int32_t i=0; i<direntry->DimensionCount; ++i )
  {
    dimentry = direntry->DimensionEntries[i];
    if( dimentry->Dimension[0] == 'X' )
      *ssX = dimentry->Size / dimentry->StoredSize;
    if( dimentry->Dimension[0] == 'Y' )
      *ssY = dimentry->Size / dimentry->StoredSize;
  }
  return true;
}

bool computeDimensions(
  SubBlockDirectorySegment  * dirsegment,
  ListOf_ImageDescriptor    * listimdesc,
  int32_t                     maxblocks
)
{
  printf( "compute dimensions...\n" );
  assert( dirsegment );

  DirectoryEntryDV * direntry;
  ImageDescriptor * imdesc;

  // create pyr 0
  listimdesc->entry = newImageDescriptor( 0, 1, 1 );
  listimdesc->next = NULL;
  listimdesc->previous = NULL;

  int32_t imax;
  if( maxblocks >= 0 )
    imax = maxblocks;
  else
    imax = dirsegment->EntryCount;

  int32_t ssx, ssy;
  // read each directory entry
  for( int32_t i=0; i<imax; ++i )
  {
    printf( "%i / %i \r", i+1, imax );
    direntry = dirsegment->Entry[i];
    findSubsampling( direntry, &ssx, &ssy );
    imdesc = findPyramid( listimdesc, direntry->PyramidType, ssx, ssy );
    updateImageDescriptor( imdesc, direntry );
  }
  printf("\n");

  // compute final sizes
  ListOf_ImageDescriptor * current = listimdesc;
  while( current )
  {
    imdesc = current->entry;
    for( int8_t j=0; j<12; ++j )
      imdesc->content[j][0] = imdesc->content[j][3] - imdesc->content[j][2];
    current = current->next;
  }
  return true;
}