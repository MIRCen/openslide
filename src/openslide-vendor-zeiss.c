/*
 * ZEISS (czi) zisraw support
 * Yaël
 *
 * In this file we implement openslide API.
 * Two functions are mandatory (zeiss_detect, zeiss_open).
 * For now, I put generic functions to manipulate zisraw format in
 * openslide-decode-zisraw.h/.c in order to have something clean, even 
 * though zisraw is only used in czi.
 */

// Empty for now