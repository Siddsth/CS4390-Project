#include "file_hash.h"

#include <openssl/evp.h>
#include <stdio.h>

int GetMD5(const void *data, size_t bytes, char hex[33])
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_bytes;
    if (EVP_Digest(data, bytes, digest, &digest_bytes, EVP_md5(), NULL) != 1 ||
        digest_bytes != 16) {
        return -1;
    }
    for (unsigned int i = 0; i < digest_bytes; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return 0;
}
