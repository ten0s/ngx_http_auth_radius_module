#ifndef __RADIUS_LIB_H__
#define __RADIUS_LIB_H__

#define RADIUS_SERVER_MAGIC_HDR 0x55AA00FF
#define RADIUS_PKG_MAX  1024

#define RADIUS_STR_INIT(str) .s = str, .len = strlen(str)

typedef struct radius_auth_t {
    unsigned char   d[16];
} radius_auth_t;

typedef struct radius_hdr_t {
    uint8_t         code;
    uint8_t         ident;
    uint16_t        len;
    radius_auth_t   auth;
} radius_hdr_t;

typedef struct radius_attr_hdr_t {
    uint8_t         type;
    uint8_t         len;
} radius_attr_hdr_t;

typedef struct radius_pkg_t {
    radius_hdr_t    hdr;
    unsigned char   attrs[RADIUS_PKG_MAX - sizeof(radius_hdr_t)];
} radius_pkg_t;

typedef struct radius_pkg_builder_t {
    radius_pkg_t   *pkg;
    unsigned char  *pos;
} radius_pkg_builder_t;

typedef enum {
    radius_attr_type_str,
    radius_attr_type_address,
    radius_attr_type_integer,
    radius_attr_type_time,
    radius_attr_type_chap_passwd,
} radius_attr_type_t;

typedef enum {
    radius_err_ok,
    radius_err_range,
    radius_err_mem,
} radius_error_t;

typedef struct radius_attr_chap_passwd_t {
    uint8_t chap_ident;
    unsigned char chap_data[16];
} radius_attr_chap_passwd_t;

typedef struct radius_attr_desc_t {
    radius_attr_type_t type;
    uint8_t            len_min;
    uint8_t            len_max;
} radius_attr_desc_t;

#define RADIUS_CODE_ACCESS_REQUEST      1
#define RADIUS_CODE_ACCESS_ACCEPT       2
#define RADIUS_CODE_ACCESS_REJECT       3
#define RADIUS_CODE_ACCESS_CHALLENGE    4

#endif // __RADIUS_LIB_H__