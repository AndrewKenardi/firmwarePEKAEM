#ifndef VERSION_CHECK_H
#define VERSION_CHECK_H

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Download isi file teks kecil (mis. berisi string versi "1.0.3")
 *        dari URL, dan simpan ke buffer. Cocok untuk file version.txt
 *        yang ukurannya cuma beberapa byte.
 */
esp_err_t version_check_fetch(const char *url, char *out_buf, size_t out_buf_size);

#endif // VERSION_CHECK_H