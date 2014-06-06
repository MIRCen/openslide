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
    g_error( "(%s:%d:%s): Could only read %li out of %li items.",
             __FILE__, __LINE__, __func__, len, count );
  return true;
}

// Converts a GUID to a printable string of format 4-2-2-2-6
// The returned string needs to be freed outside.
static char * guidToString(
  uint8_t * guid    // An array of 16 bytes containing the guid.
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
//   FIND SEGMENTS
//============================================================================

bool readNextSegmentHeader(
  FILE                * stream,
  czi_segment_header  * segmentheader,
  GError             ** err
)
{
  g_assert( stream );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

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

bool readNextSegmentHeaderWithId(
  FILE                * stream,
  czi_segment_header  * segmentheader,
  const char          * id,
  GError             ** err
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
    if( strcmp( id, segmentheader->id ) )
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
               "(%s:%d:%s): No segment %s found.",
               __FILE__, __LINE__, __func__, id );
  return false;
}

bool skipSegment(
  FILE                * stream,
  czi_segment_header  * segmentheader,
  GError             ** err
)
{
  g_assert( stream );
  g_assert( segmentheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  if( fseeko( stream, segmentheader->allocated_size, SEEK_CUR ) ) {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to seek position CUR+%li in file stream: %s",
                   __FILE__, __LINE__, __func__,
                   segmentheader->allocated_size, g_strerror(errno) );
      return false;
  }

  return true;
}

//============================================================================
//   READ SEGMENTS
//============================================================================

// TODO error handling
bool readFileHeader(
  FILE             * stream,
  czi_file_header  * fileheader,
  GError          ** err
)
{
  g_assert( stream );
  g_assert( fileheader );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)&(fileheader->major), 1, sizeof(fileheader->major), stream );
  readItems( (uint8_t*)&(fileheader->minor), 1, sizeof(fileheader->minor), stream );
  fseeko( stream, 8, SEEK_CUR );
  readItems( (uint8_t*)fileheader->primary_file_guid, 16, sizeof(fileheader->primary_file_guid[0]), stream );
  readItems( (uint8_t*)fileheader->file_guid, 16, sizeof(fileheader->file_guid[0]), stream );
  readItems( (uint8_t*)&(fileheader->file_part), 1, sizeof(fileheader->file_part), stream );
  readItems( (uint8_t*)&(fileheader->directory_position), 1, sizeof(fileheader->directory_position), stream );
  readItems( (uint8_t*)&(fileheader->metadata_position), 1, sizeof(fileheader->metadata_position), stream );
  readItems( (uint8_t*)&(fileheader->update_pending), 1, sizeof(fileheader->update_pending), stream );
  readItems( (uint8_t*)&(fileheader->attachment_directory_position), 1, sizeof(fileheader->attachment_directory_position), stream );

  return true;
}

// TODO error handling
bool readSubBlockDirectorySegment(
  FILE                            * stream,
  czi_subblock_directory_segment  * dirsegment,
  GError                        ** err
)
{
  g_assert( stream );
  g_assert( dirsegment );
  g_return_val_if_fail( err == NULL || *err == NULL, false );

  readItems( (uint8_t*)&(dirsegment->entry_count), 1, sizeof(dirsegment->entry_count), stream );
  fseeko( stream, 124, SEEK_CUR );

  dirsegment->entry = (czi_directory_entry_dv**) g_try_malloc0_n( dirsegment->entry_count, sizeof(czi_directory_entry_dv*) );
  if( dirsegment->entry == NULL )
  {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "(%s:%d:%s): Failed to allocate %i x %li bytes.",
                 __FILE__, __LINE__, __func__,
                 dirsegment->entry_count, sizeof(czi_directory_entry_dv*) );
    return false;
  }
  for( int32_t i=0; i<dirsegment->entry_count; ++i )
  {
    dirsegment->entry[i] = (czi_directory_entry_dv*) g_try_malloc0( sizeof(czi_directory_entry_dv) );
    if( dirsegment->entry[i] == NULL )
    {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                   "(%s:%d:%s): Failed to allocate %li bytes.",
                   __FILE__, __LINE__, __func__, sizeof(czi_directory_entry_dv) );
      return false;
    }
    if( !readDirectoryEntryDV( stream, dirsegment->entry[i], err ) )
    {
      g_assert( err == NULL || *err != NULL );
      return false;
    }
  }
  return true;
}

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

// TODO error handling
bool readMetadataSegment(
  FILE                  * stream,
  czi_metadata_segment  * metaseg,
  GError               ** err
)
{
  readItems( (uint8_t*)&(metaseg->xml_size), 1, sizeof(metaseg->xml_size), stream );
  readItems( (uint8_t*)&(metaseg->attachment_size), 1, sizeof(metaseg->attachment_size), stream );
  fseeko( stream, 248, SEEK_CUR );
  metaseg->xml_buf = (uint8_t*) g_try_malloc( metaseg->xml_size );
  if( metaseg->xml_buf == NULL )
  {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "(%s:%d:%s): Failed to allocate %i bytes.",
                 __FILE__, __LINE__, __func__, metaseg->xml_size );
    return false;
  }
  readItems( (uint8_t*)metaseg->xml_buf, metaseg->xml_size, sizeof(uint8_t), stream );
  // keep xml decoding here ?
  GError *tmp_err = NULL;
  metaseg->xml = _openslide_xml_parse( (const char *) metaseg->xml_buf, &tmp_err );
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

void printFileHeader( czi_file_header * fileheader )
{
  char * pf_guid = guidToString( fileheader->primary_file_guid );
  char * f_guid = guidToString( fileheader->file_guid );
  printf( "+-----------------------------------------------------------+\n" );
  printf( "|                        FileHeader                         |\n" );
  printf( "+-----------------------------------------------------------+\n" );
  printf( "| - Major: \t %i \n",                        fileheader->major );
  printf( "| - Minor: \t %i \n",                        fileheader->minor );
  printf( "| - PrimaryFileGuid: \t %s \n",              pf_guid );
  printf( "| - FileGuid: \t %s \n",                     f_guid );
  printf( "| - FilePart: \t %i \n",                     fileheader->file_part );
  printf( "| - DirectoryPosition: \t %li \n",           fileheader->directory_position );
  printf( "| - MetadataPosition: \t %li \n",            fileheader->metadata_position );
  printf( "| - UpdatePending: \t %i \n",                fileheader->update_pending );
  printf( "| - AttachmentDirectoryPosition: \t %li \n", fileheader->attachment_directory_position );
  printf( "+-----------------------------------------------------------+\n" );
  g_free( pf_guid );
  g_free( f_guid );
}

void printSubBlockDirectorySegment( czi_subblock_directory_segment * dirsegment, int32_t maxblocks )
{
  printf( "+-----------------------------------------------------------+\n" );
  printf( "|                 SubBlockDirectorySegment                  |\n" );
  printf( "+-----------------------------------------------------------+\n" );
  printf( "| - EntryCount: \t %i \n", dirsegment->entry_count );
  int32_t imax;
  if( maxblocks >= 0 )
    imax = maxblocks;
  else
    imax = dirsegment->entry_count;
  for( int32_t i=0; i<imax; ++i )
  {
    printDirectoryEntryDV( dirsegment->entry[i] );
  }
  printf( "+-----------------------------------------------------------+\n" );
}

void printDirectoryEntryDV( czi_directory_entry_dv * dirdv )
{
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| |                    DirectoryEntryDV                     |\n" );
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| | - SchemaType: \t %s \n",     dirdv->schema_type );
  printf( "| | - PixelType: \t %i \n",      dirdv->pixel_type );
  printf( "| | - FilePosition: \t %li \n",  dirdv->file_position );
  printf( "| | - FilePart: \t %i \n",       dirdv->file_part );
  printf( "| | - Compression: \t %i \n",    dirdv->compression );
  printf( "| | - PyramidType: \t %hhi \n",  dirdv->pyramid_type );
  printf( "| | - DimensionCount: \t %i \n", dirdv->dimension_count );
  for( int i=0; i<dirdv->dimension_count; ++i )
  {
    printDimensionEntryDV( dirdv->dimension_entries[i] );
  }
  printf( "| +---------------------------------------------------------+\n" );
}

void printDimensionEntryDV( czi_dimension_entry_dv * dimdv )
{
  printf( "| | +-------------------------------------------------------+\n" );
  printf( "| | |                   DimensionEntryDV                    |\n" );
  printf( "| | +-------------------------------------------------------+\n" );
  printf( "| | | - Dimension: \t %s \n",        dimdv->dimension );
  printf( "| | | - Start: \t %i \n",           dimdv->start );
  printf( "| | | - Size: \t %i \n",            dimdv->size );
  printf( "| | | - StartCoordinate: \t %f \n", dimdv->start_coordinate );
  printf( "| | | - StoredSize: \t %i \n",      dimdv->stored_size );
  printf( "| | +-------------------------------------------------------+\n" );
}

void printPyramids( czi_list_of_image_descriptor * list )
{
  printf( "+-----------------------------------------------------------+\n" );
  printf( "|                         Pyramids                          |\n" );
  printf( "+-----------------------------------------------------------+\n" );
  czi_list_of_image_descriptor * current = list;
  while( current )
  {
    printDimensions( current->entry );
    current = current->next;
  }
}

void printDimensions( czi_image_descriptor * imdesc )
{
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| |                       Dimensions                        |\n" );
  printf( "| +---------------------------------------------------------+\n" );
  printf( "| | - PyramidType: \t %hhi\n",  imdesc->pyramid_type );
  printf( "| | - SubSamplingX: \t %i\n",  imdesc->subsampling_x );
  printf( "| | - SubSamplingY: \t %i\n",  imdesc->subsampling_y );
  printf( "| | - EntryCount: \t %i\n",    imdesc->entry_count );
  printf( "| | \n" );
  printf( "| | - SizeX: \t %i\n",          imdesc->content[0][0] );
  printf( "| | - SizeY: \t %i\n",          imdesc->content[1][0] );
  printf( "| | - SizeC: \t %i\n",          imdesc->content[2][0] );
  printf( "| | - SizeZ: \t %i\n",          imdesc->content[3][0] );
  printf( "| | - SizeT: \t %i\n",          imdesc->content[4][0] );
  printf( "| | - rotations: \t %i\n",      imdesc->content[5][0] );
  printf( "| | - scenes: \t %i\n",         imdesc->content[6][0] );
  printf( "| | - illuminations: \t %i\n",  imdesc->content[7][0] );
  printf( "| | - blocks: \t %i\n",         imdesc->content[8][0] );
  printf( "| | - mosaics: \t %i\n",        imdesc->content[9][0] );
  printf( "| | - phases: \t %i\n",         imdesc->content[10][0] );
  printf( "| | - views: \t %i\n",          imdesc->content[11][0] );
  printf( "| | \n" );
  printf( "| | - tileSizeX: \t %i\n",  imdesc->content[0][1] );
  printf( "| | - tileSizeY: \t %i\n",  imdesc->content[1][1] );
  printf( "| | - tileSizeC: \t %i\n",  imdesc->content[2][1] );
  printf( "| | - tileSizeZ: \t %i\n",  imdesc->content[3][1] );
  printf( "| | - tileSizeT: \t %i\n",  imdesc->content[4][1] );
  printf( "| | - tileSizeR: \t %i\n",  imdesc->content[5][1] );
  printf( "| | - tileSizeS: \t %i\n",  imdesc->content[6][1] );
  printf( "| | - tileSizeI: \t %i\n",  imdesc->content[7][1] );
  printf( "| | - tileSizeB: \t %i\n",  imdesc->content[8][1] );
  printf( "| | - tileSizeM: \t %i\n",  imdesc->content[9][1] );
  printf( "| | - tileSizeH: \t %i\n",  imdesc->content[10][1] );
  printf( "| | - tileSizeV: \t %i\n",  imdesc->content[11][1] );
  printf( "| | \n" );
  printf( "| | - startX: \t %i\n",  imdesc->content[0][2] );
  printf( "| | - startY: \t %i\n",  imdesc->content[1][2] );
  printf( "| | - startC: \t %i\n",  imdesc->content[2][2] );
  printf( "| | - startZ: \t %i\n",  imdesc->content[3][2] );
  printf( "| | - startT: \t %i\n",  imdesc->content[4][2] );
  printf( "| | - startR: \t %i\n",  imdesc->content[5][2] );
  printf( "| | - startS: \t %i\n",  imdesc->content[6][2] );
  printf( "| | - startI: \t %i\n",  imdesc->content[7][2] );
  printf( "| | - startB: \t %i\n",  imdesc->content[8][2] );
  printf( "| | - startM: \t %i\n",  imdesc->content[9][2] );
  printf( "| | - startH: \t %i\n",  imdesc->content[10][2] );
  printf( "| | - startV: \t %i\n",  imdesc->content[11][2] );
  printf( "| +---------------------------------------------------------+\n" );
}

//============================================================================
//   FREE
//============================================================================

void freeSegmentHeader( czi_segment_header * segmentheader )
{
  g_free( segmentheader );
}

void freeFileHeader( czi_file_header * fileheader )
{
  g_free( fileheader );
}

void freeSubBlockDirectorySegment( czi_subblock_directory_segment * dirsegment )
{
  if( dirsegment )
  {
    if( dirsegment->entry )
    {
      for( int32_t i=0; i<dirsegment->entry_count; ++i )
        freeDirectoryEntryDV( dirsegment->entry[i] );
      g_free( dirsegment->entry );
    }
    g_free( dirsegment );
  }
}

void freeDirectoryEntryDV( czi_directory_entry_dv * dirdv )
{
  if( dirdv )
  {
    if( dirdv->dimension_entries )
    {
      for( int32_t i=0; i<dirdv->dimension_count; ++i )
        freeDimensionEntryDV( dirdv->dimension_entries[i] );
      g_free( dirdv->dimension_entries );
    }
    g_free( dirdv );
  }
}

void freeDimensionEntryDV( czi_dimension_entry_dv * dimdv )
{
  g_free( dimdv );
}

void freeImageDescriptor( czi_image_descriptor * imdesc )
{
  if( imdesc ) {
    freeListOf_DirectoryEntryDV( imdesc->entry_list );
    g_free( imdesc );
  }
}

void freeListOf_DirectoryEntryDV( czi_list_of_directory_entry_dv * list )
{
  if( list )
  {
    freeListOf_DirectoryEntryDV( list->next );
    g_free( list );
  }
}

void freeListOf_ImageDescriptor( czi_list_of_image_descriptor * list )
{
  if( list )
  {
    freeListOf_ImageDescriptor( list->next );
    g_free( list );
  }
}


void freeMetadataSegment( czi_metadata_segment * metaseg )
{
  if( metaseg )
  {
    if( metaseg->xml_buf )
      g_free( metaseg->xml_buf );
    if( metaseg->xml )
      xmlFreeDoc( metaseg->xml );
    g_free( metaseg );
  }
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