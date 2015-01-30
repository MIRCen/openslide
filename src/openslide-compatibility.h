/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifndef OPENSLIDE_OPENSLIDE_COMPATIBILITY_H_
#define OPENSLIDE_OPENSLIDE_COMPATIBILITY_H_

#include <stdint.h>

//////////////////////////////////////////////////////////////////////////////
/*
 * Glib2 compatibility
 */

#include <glib.h>

#define OPENSLIDE_VERSION_CONCAT(VER_MAJOR, VER_MINOR, VER_MICRO) VER_MAJOR * 10000 + VER_MINOR * 100 + VER_MICRO

#define GLIB_VERSION OPENSLIDE_VERSION_CONCAT( GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION )

// Previous patches to 2.22
#if GLIB_VERSION < 22200

gboolean
g_int64_equal (gconstpointer v1,
               gconstpointer v2);

guint
g_int64_hash (gconstpointer v);

// Previous patches to 2.16
#if GLIB_VERSION < 21600

/**
 * g_warn_if_fail:
 * @expr: the expression to check
 *
 * Logs a warning if the expression is not true.
 *
 * Since: 2.16
 */
#define g_warn_if_fail(expr) \
  do { \
    if G_LIKELY (expr) ; \
    else g_warn_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, #expr); \
  } while (0)

/* g_propagate_error then g_error_prefix on dest */
void     g_propagate_prefixed_error   (GError       **dest,
                                       GError        *src,
                                       const gchar   *format,
                                       ...) G_GNUC_PRINTF (3, 4);


/* if (err) prefix the formatted string to the ->message */
void     g_prefix_error               (GError       **err,
                                       const gchar   *format,
                                       ...) G_GNUC_PRINTF (2, 3);

void g_warn_message           (const char     *domain,
                               const char     *file,
                               int             line,
                               const char     *func,
                               const char     *warnexpr);

#define G_CHECKSUM_SHA256

typedef struct {
  /// Buffer to hold the final result and a temporary buffer for SHA256.
  union {
    uint8_t u8[64];
    uint32_t u32[16];
    uint64_t u64[8];
  } buffer;

  struct {
    uint32_t state[8];	// Internal state
    uint64_t size;	// Size of the message excluding padding
  } sha256;
} GChecksum;

GChecksum *g_checksum_new(void);
void g_checksum_update(GChecksum *ctx, const guchar *data, gssize length);
gchar *g_checksum_get_string(GChecksum *ctx);
void g_checksum_free(GChecksum *ctx);

#define SHA256_DIGEST_LENGTH 32

void SHA256_Init(GChecksum *ctx);
void SHA256_Update(GChecksum *ctx, const uint8_t *buf, size_t size);
void SHA256_Final(unsigned char out[SHA256_DIGEST_LENGTH], GChecksum *ctx);

// Previous patches to 2.14
#if GLIB_VERSION < 21400

#define HASH_IS_REAL(h_) ((h_) >= 2)

GList *
g_hash_table_get_keys          (GHashTable     *hash_table);

GList *
g_hash_table_get_values        (GHashTable *hash_table);

#endif

#endif

#endif


//////////////////////////////////////////////////////////////////////////////
/*
 *
 * Cairo compatibility
 *
 */
#include <cairo.h>

// Previous patches to 1.6
#if CAIRO_VERSION < 10600

cairo_status_t _cairo_error (cairo_status_t status);

cairo_public int
_cairo_format_bits_per_pixel (cairo_format_t format);

cairo_public int
cairo_format_stride_for_width (cairo_format_t format,
                               int            width);

#endif


//////////////////////////////////////////////////////////////////////////////
/*
 *
 * Libxml2 compatibility
 *
 */

// Previous patches to 2.7
#if LIBXML_VERSION < 20700

#include <libxml/tree.h>

xmlNodePtr XMLCALL
            xmlFirstElementChild (xmlNodePtr parent);

#endif

#endif

