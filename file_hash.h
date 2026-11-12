#ifndef FILE_HASH_H
#define FILE_HASH_H

#include <stddef.h>

int GetMD5(const void *data, size_t bytes, char hex[33]);

#endif
