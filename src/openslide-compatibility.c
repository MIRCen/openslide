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

#include <openslide-compatibility.h>

//////////////////////////////////////////////////////////////////////////////
/*
 * Glib2 compatibility
 */

// Previous patches to 2.22
#if GLIB_VERSION < 22200

typedef struct _GRealArray  GRealArray;

/**
 * GArray:
 * @data: a pointer to the element data. The data may be moved as
 *     elements are added to the #GArray.
 * @len: the number of elements in the #GArray not including the
 *     possible terminating zero element.
 *
 * Contains the public fields of a GArray.
 */
struct _GRealArray
{
  guint8 *data;
  guint   len;
  guint   alloc;
  guint   elt_size;
  guint   zero_terminated : 1;
  guint   clear : 1;
  gint    ref_count;
  GDestroyNotify clear_func;
};

typedef struct _GRealPtrArray  GRealPtrArray;

/**
 * GPtrArray:
 * @pdata: points to the array of pointers, which may be moved when the
 *     array grows
 * @len: number of pointers in the array
 *
 * Contains the public fields of a pointer array.
 */
struct _GRealPtrArray
{
  gpointer       *pdata;
  guint           len;
  guint           alloc;
  gint            ref_count;
  GDestroyNotify  element_free_func;
};

gboolean
g_int64_equal (gconstpointer v1,
	       gconstpointer v2)
{
  return *((const gint64*) v1) == *((const gint64*) v2);
}
guint
g_int64_hash (gconstpointer v)
{
   return (guint) *(const gint64*) v;
}

/**
 * g_ptr_array_set_free_func:
 * @array: A #GPtrArray
 * @element_free_func: (allow-none): A function to free elements with
 *     destroy @array or %NULL
 *
 * Sets a function for freeing each element when @array is destroyed
 * either via g_ptr_array_unref(), when g_ptr_array_free() is called
 * with @free_segment set to %TRUE or when removing elements.
 *
 * Since: 2.22
 */
void
g_ptr_array_set_free_func (GPtrArray      *array,
                           GDestroyNotify  element_free_func)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;

  g_return_if_fail (array);

  rarray->element_free_func = element_free_func;
}

/**
 * g_ptr_array_new_with_free_func:
 * @element_free_func: (allow-none): A function to free elements with
 *     destroy @array or %NULL
 *
 * Creates a new #GPtrArray with a reference count of 1 and use
 * @element_free_func for freeing each element when the array is destroyed
 * either via g_ptr_array_unref(), when g_ptr_array_free() is called with
 * @free_segment set to %TRUE or when removing elements.
 *
 * Returns: A new #GPtrArray
 *
 * Since: 2.22
 */
GPtrArray*
g_ptr_array_new_with_free_func (GDestroyNotify element_free_func)
{
  GPtrArray *array;

  array = g_ptr_array_new ();
  g_ptr_array_set_free_func (array, element_free_func);

  return array;
}

// Previous patches to 2.16
#if GLIB_VERSION < 21600

#include <string.h>

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

G_GNUC_PRINTF(2, 0)
static void
g_error_add_prefix (gchar       **string,
                    const gchar  *format,
                    va_list       ap)
{
  gchar *oldstring;
  gchar *prefix;

  prefix = g_strdup_vprintf (format, ap);
  oldstring = *string;
  *string = g_strconcat (prefix, oldstring, NULL);
  g_free (oldstring);
  g_free (prefix);
}

/**
 * g_propagate_prefixed_error:
 * @dest: error return location
 * @src: error to move into the return location
 * @format: printf()-style format string
 * @...: arguments to @format
 *
 * If @dest is %NULL, free @src; otherwise, moves @src into *@dest.
 * *@dest must be %NULL. After the move, add a prefix as with
 * g_prefix_error().
 *
 * Since: 2.16
 **/
void
g_propagate_prefixed_error (GError      **dest,
                            GError       *src,
                            const gchar  *format,
                            ...)
{
  g_propagate_error (dest, src);

  if (dest && *dest)
    {
      va_list ap;

      va_start (ap, format);
      g_error_add_prefix (&(*dest)->message, format, ap);
      va_end (ap);
    }
}

/**
 * g_prefix_error:
 * @err: (allow-none): a return location for a #GError, or %NULL
 * @format: printf()-style format string
 * @...: arguments to @format
 *
 * Formats a string according to @format and prefix it to an existing
 * error message. If @err is %NULL (ie: no error variable) then do
 * nothing.
 *
 * If *@err is %NULL (ie: an error variable is present but there is no
 * error condition) then also do nothing. Whether or not it makes sense
 * to take advantage of this feature is up to you.
 *
 * Since: 2.16
 */
void
g_prefix_error (GError      **err,
                const gchar  *format,
                ...)
{
  if (err && *err)
    {
      va_list ap;

      va_start (ap, format);
      g_error_add_prefix (&(*err)->message, format, ap);
      va_end (ap);
    }
}

void
g_warn_message (const char     *domain,
                const char     *file,
                int             line,
                const char     *func,
                const char     *warnexpr)
{
  char *s, lstr[32];
  g_snprintf (lstr, 32, "%d", line);
  if (warnexpr)
    s = g_strconcat ("(", file, ":", lstr, "):",
                     func, func[0] ? ":" : "",
                     " runtime check failed: (", warnexpr, ")", NULL);
  else
    s = g_strconcat ("(", file, ":", lstr, "):",
                     func, func[0] ? ":" : "",
                     " ", "code should not be reached", NULL);
  g_log (domain, G_LOG_LEVEL_WARNING, "%s", s);
  g_free (s);
}

//#define Checksum SHA256_CTX
//#define CHECKSUM_SHA256

// At least on x86, GCC is able to optimize this to a rotate instruction.
#define rotr_32(num, amount) ((num) >> (amount) | (num) << (32 - (amount)))

#define blk0(i) (W[i] = data[i])
#define blk2(i) (W[i & 15] += s1(W[(i - 2) & 15]) + W[(i - 7) & 15] \
                + s0(W[(i - 15) & 15]))

#define Ch(x, y, z) (z ^ (x & (y ^ z)))
#define Maj(x, y, z) ((x & y) | (z & (x | y)))

#define a(i) T[(0 - i) & 7]
#define b(i) T[(1 - i) & 7]
#define c(i) T[(2 - i) & 7]
#define d(i) T[(3 - i) & 7]
#define e(i) T[(4 - i) & 7]
#define f(i) T[(5 - i) & 7]
#define g(i) T[(6 - i) & 7]
#define h(i) T[(7 - i) & 7]

#define R(i) h(i) += S1(e(i)) + Ch(e(i), f(i), g(i)) + SHA256_K[i + j] \
             + (j ? blk2(i) : blk0(i)); \
             d(i) += h(i); \
             h(i) += S0(a(i)) + Maj(a(i), b(i), c(i))

#define S0(x) (rotr_32(x, 2) ^ rotr_32(x, 13) ^ rotr_32(x, 22))
#define S1(x) (rotr_32(x, 6) ^ rotr_32(x, 11) ^ rotr_32(x, 25))
#define s0(x) (rotr_32(x, 7) ^ rotr_32(x, 18) ^ (x >> 3))
#define s1(x) (rotr_32(x, 17) ^ rotr_32(x, 19) ^ (x >> 10))

static const uint32_t SHA256_K[64] = {
  0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
  0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
  0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
  0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
  0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
  0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
  0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
  0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
  0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
  0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
  0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
  0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
  0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
  0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
  0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
  0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

static void transform(uint32_t state[8], const uint32_t data[16])
{
  uint32_t W[16];
  uint32_t T[8];

  // Copy state[] to working vars.
  memcpy(T, state, sizeof(T));

  // 64 operations, partially loop unrolled
  for (unsigned int j = 0; j < 64; j += 16) {
    R( 0); R( 1); R( 2); R( 3);
    R( 4); R( 5); R( 6); R( 7);
    R( 8); R( 9); R(10); R(11);
    R(12); R(13); R(14); R(15);
  }

  // Add the working vars back into state[].
  state[0] += a(0);
  state[1] += b(0);
  state[2] += c(0);
  state[3] += d(0);
  state[4] += e(0);
  state[5] += f(0);
  state[6] += g(0);
  state[7] += h(0);
}

static void process(GChecksum *ctx)
{
  uint32_t data[16];

  for (size_t i = 0; i < 16; ++i)
    data[i] = GUINT32_TO_BE(ctx->buffer.u32[i]);

  transform(ctx->sha256.state, data);
}

void SHA256_Init(GChecksum *ctx)
{
  static const uint32_t s[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
  };

  memcpy(ctx->sha256.state, s, sizeof(s));
  ctx->sha256.size = 0;
}


void SHA256_Update(GChecksum *ctx, const uint8_t *buf, size_t size)
{
  // Copy the input data into a properly aligned temporary buffer.
  // This way we can be called with arbitrarily sized buffers
  // (no need to be multiple of 64 bytes), and the code works also
  // on architectures that don't allow unaligned memory access.
  while (size > 0) {
    const size_t copy_start = ctx->sha256.size & 0x3F;
    size_t copy_size = 64 - copy_start;
    if (copy_size > size)
      copy_size = size;

    memcpy(ctx->buffer.u8 + copy_start, buf, copy_size);

    buf += copy_size;
    size -= copy_size;
    ctx->sha256.size += copy_size;

    if ((ctx->sha256.size & 0x3F) == 0)
      process(ctx);
  }
}

void SHA256_Final(unsigned char out[SHA256_DIGEST_LENGTH], GChecksum *ctx)
{
  // Add padding as described in RFC 3174 (it describes SHA-1 but
  // the same padding style is used for SHA-256 too).
  size_t pos = ctx->sha256.size & 0x3F;
  ctx->buffer.u8[pos++] = 0x80;

  while (pos != 64 - 8) {
    if (pos == 64) {
      process(ctx);
      pos = 0;
    }

    ctx->buffer.u8[pos++] = 0x00;
  }

  // Convert the message size from bytes to bits.
  ctx->sha256.size *= 8;

  ctx->buffer.u64[(64 - 8) / 8] = GUINT64_TO_BE(ctx->sha256.size);

  process(ctx);

  for (size_t i = 0; i < 8; ++i)
    ctx->buffer.u32[i] = GUINT_TO_BE(ctx->sha256.state[i]);

  memcpy(out, ctx->buffer.u8, SHA256_DIGEST_LENGTH);
}

GChecksum *g_checksum_new(void)
{
  GChecksum *ctx = g_slice_new(GChecksum);
  SHA256_Init(ctx);
  return ctx;
}

void g_checksum_update(GChecksum *ctx, const guchar *data, gssize length)
{
  SHA256_Update(ctx, data, length);
}

static const gchar _tohex[] = "0123456789abcdef";
gchar *g_checksum_get_string(GChecksum *ctx)
{
  static gchar hexdigest[SHA256_DIGEST_LENGTH*2+1];
  guchar digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, ctx);
  for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    hexdigest[i*2] = _tohex[(digest[i] & 0xf0) >> 4];
    hexdigest[i*2+1] = _tohex[(digest[i] & 0x0f)];
  }
  hexdigest[SHA256_DIGEST_LENGTH*2] = '\0';
  return hexdigest;
}

void g_checksum_free(GChecksum *ctx)
{
  g_slice_free(GChecksum, ctx);
}


// Previous patches to 2.14
#if GLIB_VERSION < 21400

struct _GHashTable
{
  gint             size;
  gint             mod;
  guint            mask;
  gint             nnodes;
  gint             noccupied;  /* nnodes + tombstones */

  gpointer        *keys;
  guint           *hashes;
  gpointer        *values;

  GHashFunc        hash_func;
  GEqualFunc       key_equal_func;
  gint             ref_count;
#ifndef G_DISABLE_ASSERT
  /*
   * Tracks the structure of the hash table, not its contents: is only
   * incremented when a node is added or removed (is not incremented
   * when the key or data of a node is modified).
   */
  int              version;
#endif
  GDestroyNotify   key_destroy_func;
  GDestroyNotify   value_destroy_func;
};

/**
 * g_hash_table_get_values:
 * @hash_table: a #GHashTable
 *
 * Retrieves every value inside @hash_table. The returned data
 * is valid until @hash_table is modified.
 *
 * Returns: a #GList containing all the values inside the hash
 *     table. The content of the list is owned by the hash table and
 *     should not be modified or freed. Use g_list_free() when done
 *     using the list.
 *
 * Since: 2.14
 */
GList *
g_hash_table_get_values (GHashTable *hash_table)
{
  gint i;
  GList *retval;

  g_return_val_if_fail (hash_table != NULL, NULL);

  retval = NULL;
  for (i = 0; i < hash_table->size; i++)
    {
      if (HASH_IS_REAL (hash_table->hashes[i]))
        retval = g_list_prepend (retval, hash_table->values[i]);
    }

  return retval;
}

/**
 * g_hash_table_get_keys:
 * @hash_table: a #GHashTable
 *
 * Retrieves every key inside @hash_table. The returned data is valid
 * until changes to the hash release those keys.
 *
 * Returns: a #GList containing all the keys inside the hash
 *     table. The content of the list is owned by the hash table and
 *     should not be modified or freed. Use g_list_free() when done
 *     using the list.
 *
 * Since: 2.14
 */
GList *
g_hash_table_get_keys (GHashTable *hash_table)
{
  gint i;
  GList *retval;

  g_return_val_if_fail (hash_table != NULL, NULL);

  retval = NULL;
  for (i = 0; i < hash_table->size; i++)
    {
      if (HASH_IS_REAL (hash_table->hashes[i]))
        retval = g_list_prepend (retval, hash_table->keys[i]);
    }

  return retval;
}

#endif

#endif

#endif


//////////////////////////////////////////////////////////////////////////////
/*
 *
 * Cairo compatibility
 *
 */

// Previous patches to 1.6
#if CAIRO_VERSION < 10600

#include <assert.h>

#ifndef INT32_MAX
# define INT32_MAX	(2147483647)
#endif

#ifndef UINT32_MAX
# define UINT32_MAX     (4294967295U)
#endif

#define CAIRO_FORMAT_VALID(format) ((format) >= CAIRO_FORMAT_ARGB32 &&		\
                                    (format) <= CAIRO_FORMAT_RGB16_565)

#define CAIRO_FORMAT_INVALID   (cairo_format_t)-1

/* pixman-required stride alignment in bytes. */
#define CAIRO_STRIDE_ALIGNMENT (sizeof (uint32_t))
#define CAIRO_STRIDE_FOR_WIDTH_BPP(w,bpp) \
   ((((bpp)*(w)+7)/8 + CAIRO_STRIDE_ALIGNMENT-1) & -CAIRO_STRIDE_ALIGNMENT)

/* hide compiler warnings when discarding the return value */
#define _cairo_error_throw(status) do { \
    cairo_status_t status__ = _cairo_error (status); \
    (void) status__; \
} while (0)

#define ASSERT_NOT_REACHED		\
do {					\
    assert (!"reached");		\
} while (0)

#if defined(_MSC_VER) && defined(_M_IX86)
/* When compiling with /Gy and /OPT:ICF identical functions will be folded in together.
   The CAIRO_ENSURE_UNIQUE macro ensures that a function is always unique and
   will never be folded into another one. Something like this might eventually
   be needed for GCC but it seems fine for now. */
#define CAIRO_ENSURE_UNIQUE                       \
    do {                                          \
	char file[] = __FILE__;                   \
	__asm {                                   \
	    __asm jmp __internal_skip_line_no     \
	    __asm _emit (__COUNTER__ & 0xff)      \
	    __asm _emit ((__COUNTER__>>8) & 0xff) \
	    __asm _emit ((__COUNTER__>>16) & 0xff)\
	    __asm _emit ((__COUNTER__>>24) & 0xff)\
	    __asm lea eax, dword ptr file         \
	    __asm __internal_skip_line_no:        \
	};                                        \
    } while (0)
#else
#define CAIRO_ENSURE_UNIQUE    do { } while (0)
#endif

#define _cairo_status_is_error(status) \
    (status != CAIRO_STATUS_SUCCESS && status <= CAIRO_STATUS_INVALID_DSC_COMMENT)
    
/**
 * _cairo_error:
 * @status: a status value indicating an error, (eg. not
 * %CAIRO_STATUS_SUCCESS)
 *
 * Checks that status is an error status, but does nothing else.
 *
 * All assignments of an error status to any user-visible object
 * within the cairo application should result in a call to
 * _cairo_error().
 *
 * The purpose of this function is to allow the user to set a
 * breakpoint in _cairo_error() to generate a stack trace for when the
 * user causes cairo to detect an error.
 *
 * Return value: the error status.
 **/
cairo_status_t
_cairo_error (cairo_status_t status)
{
    CAIRO_ENSURE_UNIQUE;
    assert (_cairo_status_is_error (status));

    return status;
}

    int
_cairo_format_bits_per_pixel (cairo_format_t format)
{
    switch (format) {
    case CAIRO_FORMAT_ARGB32:
    case CAIRO_FORMAT_RGB24:
	return 32;
    case CAIRO_FORMAT_RGB16_565:
	return 16;
    case CAIRO_FORMAT_A8:
	return 8;
    case CAIRO_FORMAT_A1:
	return 1;
    case CAIRO_FORMAT_INVALID:
    default:
	ASSERT_NOT_REACHED;
	return 0;
    }
}

/**
 * cairo_format_stride_for_width:
 * @format: A #cairo_format_t value
 * @width: The desired width of an image surface to be created.
 *
 * This function provides a stride value that will respect all
 * alignment requirements of the accelerated image-rendering code
 * within cairo. Typical usage will be of the form:
 *
 * <informalexample><programlisting>
 * int stride;
 * unsigned char *data;
 * cairo_surface_t *surface;
 *
 * stride = cairo_format_stride_for_width (format, width);
 * data = malloc (stride * height);
 * surface = cairo_image_surface_create_for_data (data, format,
 *						  width, height,
 *						  stride);
 * </programlisting></informalexample>
 *
 * Return value: the appropriate stride to use given the desired
 * format and width, or -1 if either the format is invalid or the width
 * too large.
 *
 * Since: 1.6
 **/
    int
cairo_format_stride_for_width (cairo_format_t	format,
			       int		width)
{
    int bpp;

    if (! CAIRO_FORMAT_VALID (format)) {
	_cairo_error_throw (CAIRO_STATUS_INVALID_FORMAT);
	return -1;
    }

    bpp = _cairo_format_bits_per_pixel (format);
    if ((unsigned) (width) >= (INT32_MAX - 7) / (unsigned) (bpp))
	return -1;

    return CAIRO_STRIDE_FOR_WIDTH_BPP (width, bpp);
}

#endif


//////////////////////////////////////////////////////////////////////////////
/*
 *
 * Libxml2 compatibility
 *
 */

// Previous patches to 2.7
#if LIBXML_VERSION < 20700

/**
 * xmlFirstElementChild:
 * @parent: the parent node
 *
 * Finds the first child node of that element which is a Element node
 * Note the handling of entities references is different than in
 * the W3C DOM element traversal spec since we don't have back reference
 * from entities content to entities references.
 *
 * Returns the first element child or NULL if not available
 */
xmlNodePtr
xmlFirstElementChild(xmlNodePtr parent) {
    xmlNodePtr cur = NULL;

    if (parent == NULL)
        return(NULL);
    switch (parent->type) {
        case XML_ELEMENT_NODE:
        case XML_ENTITY_NODE:
        case XML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_HTML_DOCUMENT_NODE:
            cur = parent->children;
            break;
        default:
            return(NULL);
    }
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE)
            return(cur);
        cur = cur->next;
    }
    return(NULL);
}

#endif
