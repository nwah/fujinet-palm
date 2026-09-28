#ifndef FN_TYPES_H
#define FN_TYPES_H

#ifdef FN_HAVE_STDINT
#include <stdint.h>
typedef uint8_t  fn_u8;
typedef uint16_t fn_u16;
typedef uint32_t fn_u32;
typedef int16_t  fn_i16;
typedef int32_t  fn_i32;
#else
typedef unsigned char  fn_u8;
typedef unsigned short fn_u16;
typedef unsigned long  fn_u32;
typedef short          fn_i16;
typedef long           fn_i32;
#endif

typedef fn_u8 fn_bool;
#define FN_TRUE  ((fn_bool)1)
#define FN_FALSE ((fn_bool)0)

typedef enum {
    FN_OK = 0,
    FN_ERR_TIMEOUT,   /* no reply within deadline */
    FN_ERR_NAK,       /* device replied NAK */
    FN_ERR_CHECKSUM,  /* reply checksum mismatch */
    FN_ERR_LENGTH,    /* malformed/short/oversized frame */
    FN_ERR_DEVICE,    /* ACK arrived from a device other than the one we called */
    FN_ERR_IO,        /* transport-level error */
    FN_ERR_OVERFLOW,  /* reply payload larger than caller's buffer (data is truncated but copied) */
    FN_ERR_PARAM      /* bad arguments passed by caller (e.g. value too large, unit out of range) */
} FnErr;

#endif /* FN_TYPES_H */
