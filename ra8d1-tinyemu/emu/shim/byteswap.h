/*
 * byteswap.h - freestanding stand-in for the glibc header.
 *
 * cutils.h:87-95 provides an inline bswap_32() only under _WIN32 and
 * otherwise does #include <byteswap.h>, which is a glibc-ism that does not
 * exist in newlib or picolibc. Putting this on the include path satisfies
 * that include with no edit to the vendored TinyEMU sources.
 *
 * Only bswap_32 is actually reached (cpu_to_be32), but all three are here so
 * a future file that uses the others does not fail at link time.
 */
#ifndef SHIM_BYTESWAP_H
#define SHIM_BYTESWAP_H

#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)

#endif /* SHIM_BYTESWAP_H */
