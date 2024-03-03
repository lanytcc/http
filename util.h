
#ifndef LANYT_UTIL_H
#define LANYT_UTIL_H

size_t calculate_encoded_size(const char *src);
void urlencode(const char *src, char *dst, size_t dst_max_len);
void urldecode(const char *src, char *dst, size_t dst_max_len);

#endif // LANYT_UTIL_H