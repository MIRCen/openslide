/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_JXR_H_
#define OPENSLIDE_OPENSLIDE_DECODE_JXR_H_

#include <stdint.h>
#include <glib.h>

/* JPEG XR support */
bool _openslide_jxr_decode_buffer( const void       * data    G_GNUC_UNUSED,
                                   uint32_t           datalen G_GNUC_UNUSED,
                                   uint32_t         * dest    G_GNUC_UNUSED,
                                   int32_t            w       G_GNUC_UNUSED,
                                   int32_t            h       G_GNUC_UNUSED,
                                   GError          ** err     G_GNUC_UNUSED );

#endif
