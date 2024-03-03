
#include "util.h"

#include <stdio.h>

// 判断字符是否需要编码
static int is_char_needs_encoding(unsigned char c) {
    return !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' ||
             c >= '0' && c <= '9' || c == '-' || c == '_' || c == '.' ||
             c == '~');
}

// URL编码一个字节
static void urlencode_char(unsigned char c, char *output) {
    sprintf(output, "%%%02X", c);
}

// 解析两个十六进制字符为一个字节
static unsigned char hex_to_char(char high, char low) {
    return ((high >= 'A' ? (high & 0xDF) - 'A' + 10 : high - '0') << 4) |
           (low >= 'A' ? (low & 0xDF) - 'A' + 10 : low - '0');
}

// 计算URL编码后的字符串所需的内存大小
size_t calculate_encoded_size(const char *src) {
    size_t size = 0;
    while (*src) {
        size += is_char_needs_encoding(*src++) ? 3 : 1;
    }
    return size;
}

// URL编码, 超过dst_max_len长度的部分会被截断
void urlencode(const char *src, char *dst, size_t dst_max_len) {
    while (*src && dst_max_len > 1) {
        if (is_char_needs_encoding(*src)) {
            if (dst_max_len < 4)
                break;
            urlencode_char(*src++, dst);
            dst += 3;
            dst_max_len -= 3;
        } else {
            *dst++ = *src++;
            dst_max_len--;
        }
    }
    *dst = '\0';
}

// URL解码，超过dst_max_len长度的部分会被截断
void urldecode(const char *src, char *dst, size_t dst_max_len) {
    while (*src && dst_max_len-- > 1) {
        if (*src == '%' && *(src + 1) && *(src + 2)) {
            *dst++ = hex_to_char(src[1], src[2]);
            src += 3;
        } else {
            *dst++ = *src == '+' ? ' ' : *src;
            src++;
        }
    }
    *dst = '\0';
}
