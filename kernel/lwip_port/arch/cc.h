#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* i386 is little-endian; lwIP provides htons/htonl from this. */
#define BYTE_ORDER LITTLE_ENDIAN

/* Struct packing for protocol headers (GCC). */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/* Diagnostics / asserts route into the kernel log + panic. */
void lwip_port_diag(const char* fmt, ...);
void lwip_port_assert_fail(const char* msg, const char* file, int line);

#define LWIP_PLATFORM_DIAG(x)   do { lwip_port_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { lwip_port_assert_fail((x), __FILE__, __LINE__); } while (0)

/* No errno.h in this freestanding libc: let lwIP define its own. */
#define LWIP_PROVIDE_ERRNO 1

/* No inttypes.h or ctype.h in freestanding toolchain: suppress both
 * includes; lwIP has built-in fallbacks for ctype.h, and we supply the
 * printf format specifiers for u8/u16/u32/size_t below. */
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H    1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

#endif /* LWIP_ARCH_CC_H */
