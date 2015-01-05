/*
 * ZEISS (czi) zisraw support
 * Yaël
 *
 * Let's use our czi/zisraw functions !
 */

//--- openslide --------------------------------------------------------------
//#include <openslide-decode-zisraw.h>
#include <openslide-private.h>
#include <openslide-error.h>
#include <openslide.h>
//#include "openslide-tools-common.h"

//--- std --------------------------------------------------------------------
#include <stdio.h>                    // FILE
#include <string.h>
#include <stdlib.h>                   // alloc/free (TODO replace with glib)
#include <limits.h>                   // fmin/fmax (TODO change)
#include <glib.h>                     // GError
#include <png.h>
#include <inttypes.h>
#include <glib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>

// Ugly inclusion to be able to use some functions for testing
#include <openslide-vendor-zeiss.c>

//--- preprocessing stuff ----------------------------------------------------
#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x100)
//----------------------------------------------------------------------------

static const char SOFTWARE[] = "Software";
static const char OPENSLIDE[] = "OpenSlide <http://openslide.org/>";

static void fail(const char *format, ...) {
  va_list ap;

  va_start(ap, format);
  char *msg = g_strdup_vprintf(format, ap);
  va_end(ap);

  fprintf(stderr, "%s: %s\n", g_get_prgname(), msg);
  fflush(stderr);

  exit(1);
}

static bool czi_write_tile( struct _czi     * czi,
                            FILE            * f,
                            int32_t           level,
                            int64_t           uid,
                            GError         ** err ) {
  int32_t buffer_size = 0;
  uint8_t * buffer;

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

  struct _openslide_czi_tile_descriptor * tile_descriptor = czi_new_tile_descriptor(tile, err);
  int32_t h = tile_descriptor->size_y / s_level->subsampling_y,
          w = tile_descriptor->size_x / s_level->subsampling_x;
  czi_free_tile_descriptor(tile_descriptor);

  // Load tile
  buffer = _openslide_czi_load_tile( czi,
                                     level,
                                     uid,
                                     &buffer_size,
                                     err );

  if (!buffer) {
    g_debug("unable to allocate buffer for tile uid: %ld", uid);
    return false;
  }

  fprintf(stdout, "czi_write_tiles - buffer_size: %d\n", buffer_size);
  fprintf(stdout, "czi_write_tiles - height: %d, width: %d\n", h, w);
  fflush(stdout);

  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
            NULL, NULL, NULL);
  if (!png_ptr) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Failed to initialize PNG" );
    return false;
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Failed to initialize PNG" );
    return false;
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
            "Failed to write PNG" );
    return false;
  }

  png_init_io(png_ptr, f);

  png_set_IHDR(png_ptr, info_ptr, w, h, 8,
         PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
         PNG_COMPRESSION_TYPE_DEFAULT,
         PNG_FILTER_TYPE_DEFAULT);

  // text
  png_text text_ptr[1];
  memset(text_ptr, 0, sizeof text_ptr);
  text_ptr[0].compression = PNG_TEXT_COMPRESSION_NONE;
  char *key = strdup(SOFTWARE);
  text_ptr[0].key = key;
  char *text = strdup(OPENSLIDE);
  text_ptr[0].text = text;
  text_ptr[0].text_length = strlen(text);

  png_set_text(png_ptr, info_ptr, text_ptr, 1);

  // start writing
  png_write_info(png_ptr, info_ptr);

  uint8_t *p = (uint8_t *)buffer;
  uint8_t *dest = g_malloc(w * 4);
  int32_t lines_to_draw = h;

  while (lines_to_draw) {

    for (int i = 0; i < w; i++) {
      uint8_t *p8 = (uint8_t *) (dest + (i * 4));

      uint8_t a = 255;
      uint8_t r = p[2];
      uint8_t g = p[1];
      uint8_t b = p[0];

      p8[0] = r;
      p8[1] = g;
      p8[2] = b;
      p8[3] = a;

      p = (uint8_t *) (p + 3);
    }


    png_write_row(png_ptr, (png_bytep) dest);
    lines_to_draw--;
  }

  // end
  g_free(dest);
  g_free(key);
  g_free(text);
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);

  return true;
}

int main( int argc, const char **argv )
{
  g_debug( "Is big endian: %i", (int)IS_BIG_ENDIAN );
  GError * err = NULL;

  //--------------------------------------------------------------------------
  // variables
  //--------------------------------------------------------------------------
  char * filename                   = NULL;
  char * xmlout                     = NULL;
  int    displaysources             = 1;
  int    displaylevels              = 1;
  int    displaytiles               = 1;
  int    displaydimensions          = 1;
  int    displaytiledescriptors     = 1;
  int    startlevel                 = 0;
  int    endlevel                   = -1;
  int    writetiles                 = 0;

  //--------------------------------------------------------------------------
  // read options
  //--------------------------------------------------------------------------
  for( int i=0; i < argc; ++i )
  {
    if( !strcmp( argv[i], "-i" ) )
    {
      ++i;
      filename = malloc( strlen(argv[i]) );
      strcpy( filename, argv[i] );
    }
    else if( !strcmp( argv[i], "--displaysources" ) )
    {
      displaysources = 1;
    }
    else if( !strcmp( argv[i], "--displaylevels" ) )
    {
      displaylevels = 1;
    }
    else if( !strcmp( argv[i], "--displaytiles" ) )
    {
      displaytiles = 1;
    }
    else if( !strcmp( argv[i], "--displaydimensions" ) )
    {
      displaydimensions = 1;
    }
    else if( !strcmp( argv[i], "--displaytiledescriptors" ) )
    {
      displaytiledescriptors = 1;
    }
    else if( !strcmp( argv[i], "--writetiles" ) )
    {
      writetiles = 1;
    }
    else if( !strcmp( argv[i], "--startlevel" ) )
    {
      if (i + 1 < argc) {
        startlevel = strtol(argv[++i], NULL, 10);
        g_debug("startlevel: %d", startlevel);
      }
      else {
        g_warning( "Missing --startlevel option value." );
        return EXIT_FAILURE;
      }
    }
    else if( !strcmp( argv[i], "--endlevel" ) )
    {
      if (i + 1 < argc) {
        endlevel = strtol(argv[++i], NULL, 10);
        g_debug("endlevel: %d", endlevel);
      }
      else {
        g_warning( "Missing --endlevel option value." );
        return EXIT_FAILURE;
      }
    }
  }

  //--------------------------------------------------------------------------
  // check options
  //--------------------------------------------------------------------------
  if( !filename ) {
    g_warning( "Missing -i option." );
    return EXIT_FAILURE;
  }

  //--------------------------------------------------------------------------
  // initialize czi
  //--------------------------------------------------------------------------
  gpointer key, value;
  struct _czi        * czi = _openslide_czi_decode( filename, &err );

  int treelevel = 0;
  if (endlevel < 0)
    endlevel = czi->levels->len - 1;

  //--------------------------------------------------------------------------
  // display sources
  //--------------------------------------------------------------------------

  for( uint32_t s = 0; s < czi->sources->len; ++s ) {

    // Display source information
    if( displaysources )
      czi_display_source( (struct _czi_source*) g_ptr_array_index( czi->sources, s ), CZI_DISPLAY_INDENT * treelevel );

    // Display level information
    if( displaylevels )
      treelevel++;

    for( uint32_t l = (uint32_t)startlevel; l <= (uint32_t)endlevel; ++l ) {
      struct _czi_level * level = (struct _czi_level*) g_ptr_array_index( czi->levels, l );
      if( displaylevels )
        czi_display_level( level, CZI_DISPLAY_INDENT * treelevel );

      //g_debug("display tiles");
      // Display tile information
      GHashTableIter titer;
      g_hash_table_iter_init (&titer, level->tiles);

      if (displaytiles)
        treelevel++;

      while (g_hash_table_iter_next (&titer, &key, &value))  {
        struct _czi_tile * tile = (struct _czi_tile*) value;
        if( displaytiles )
        {
          czi_display_tile( tile, CZI_DISPLAY_INDENT * treelevel );
        }

        // Display dimension information
        GHashTableIter diter;
        g_hash_table_iter_init (&diter, tile->dimensions);

        if (displaydimensions)
          treelevel++;

        while (g_hash_table_iter_next (&diter, &key, &value)) {
          struct _czi_dimension * dimension = (struct _czi_dimension*) value;
          if( displaydimensions )
            czi_display_dimension( dimension, CZI_DISPLAY_INDENT * treelevel );
        }

        if (displaydimensions)
          treelevel--;

        if (displaytiledescriptors)
          treelevel++;

        if (displaytiledescriptors) {
          struct _openslide_czi_tile_descriptor * tile_descriptor = czi_new_tile_descriptor(tile, &err);
          czi_display_tile_descriptor(tile_descriptor, CZI_DISPLAY_INDENT * treelevel );
          czi_free_tile_descriptor(tile_descriptor);
        }

        if (writetiles) {

         struct _openslide_czi_tile_descriptor * tile_descriptor = czi_new_tile_descriptor(tile, &err);

          // set up output file
          char output[256] = { 0 };
          sprintf( output, "level_%d_tile_%ld.png", l, tile->uid);
          FILE *png = _openslide_fopen(output, "wb", &err);

          if (!png) {
            fail("Can't open %s for writing: %s", output,
           strerror(errno));
          }

          if (!czi_write_tile(czi, png, l, tile->uid, &err)) {
            g_debug(err->message);
            break;
          }
          fclose(png);
          czi_free_tile_descriptor(tile_descriptor);

        }

        if ( displaytiledescriptors )
          treelevel--;
      }

      if ( displaytiles )
        treelevel--;
    }

    if ( displaylevels )
      treelevel--;
  }

  return EXIT_SUCCESS;
}
