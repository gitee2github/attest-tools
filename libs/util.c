/*
 * Copyright (C) 2018-2019 Huawei Technologies Duesseldorf GmbH
 *
 * Author: Roberto Sassu <roberto.sassu@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * File: util.c
 *       Miscellaneous functions.
 */

/**
 * @defgroup util-api Util API
 * @ingroup developer-api
 * @brief
 * Miscellaneous functions.
 */

/**
 * \addtogroup util-api
 *  @{
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/mman.h>

#include <curl/curl.h>

#include <openssl/evp.h>

#include "util.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000
#define EVP_ENCODE_CTX_new() OPENSSL_malloc(sizeof(EVP_ENCODE_CTX))
#define EVP_ENCODE_CTX_free(ctx) OPENSSL_free(ctx)
#endif

#define DECODED_BLOCK_SIZE 48
#define ENCODED_BLOCK_SIZE 65

int attest_util_read_file(const char *path, size_t *len, unsigned char **data)
{
    struct stat st;
    int rc = 0, fd;

    if (stat(path, &st) == -1)
        return -EACCES;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -EACCES;

    *len = st.st_size;

    *data = mmap(NULL, *len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (*data == MAP_FAILED)
        rc = -ENOMEM;

    close(fd);
    return rc;
}

int attest_util_read_seq_file(const char *path, size_t *len,
                  unsigned char **data)
{
    unsigned char buf[512], *buf_ptr = buf;
    size_t total_len = 0, cur_len, buf_len = sizeof(buf);
    int rc = 0, fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -EACCES;

    *data = NULL;
again:
    if (*data) {
        close(fd);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            free (*data);
            return -EACCES;
        }
    }

    while (buf_len) {
        cur_len = read(fd, buf_ptr, buf_len);
        if (cur_len <= 0) {
            if (*data) {
                if (buf_len)
                    return -EIO;

                break;
            }

            if (!total_len)
                return -EIO;

            *data = malloc(total_len + 1);
            if (!*data)
                return -ENOMEM;

            (*data)[total_len] = '\0';

            buf_ptr = *data;
            buf_len = *len = total_len;
            goto again;
        }

        if (*data) {
            buf_ptr += cur_len;
            buf_len -= cur_len;
        } else {
            total_len += cur_len;
        }
    }

    close(fd);
    return rc;
}

int attest_util_write_file(const char *path, size_t len, unsigned char *data,
               int append)
{
    int open_flags = O_WRONLY | O_CREAT;
    int rc, fd;

    open_flags |= (append) ? O_APPEND : O_TRUNC;

    fd = open(path, open_flags, 0600);
    if (!fd)
        return -EACCES;

    rc = attest_util_write_buf(fd, data, len);
    close(fd);
    return rc;
}

int attest_util_copy_file(const char *path_source, const char *path_dest)
{
    unsigned char *data;
    size_t len;
    int rc;

    rc = attest_util_read_file(path_source, &len, &data);
    if (rc)
        return rc;

    rc = attest_util_write_file(path_dest, len, data, 0);
    munmap(data, len);
    return rc;
}

static int attest_util_rw_buf(int fd, unsigned char *buf, size_t buf_len,
                  int op)
{
    size_t processed = 0, cur_processed;

    while (processed < buf_len) {
        if (op == O_RDONLY)
            cur_processed = read(fd, buf + processed,
                         buf_len - processed);
        else
            cur_processed = write(fd, buf + processed,
                          buf_len - processed);
        if (cur_processed <= 0)
            return -EIO;

        processed += cur_processed;
    }

    return 0;
}

int attest_util_read_buf(int fd, unsigned char *buf, size_t buf_len)
{
    return attest_util_rw_buf(fd, buf, buf_len, O_RDONLY);
}

int attest_util_write_buf(int fd, unsigned char *buf, size_t buf_len)
{
    return attest_util_rw_buf(fd, buf, buf_len, O_WRONLY);
}

int attest_util_calc_digest(const char *algo, int *digest_len,
                unsigned char *digest, int len, void *data)
{
    EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
    const EVP_MD *md = EVP_get_digestbyname(algo);
    int rc = -EINVAL;

    if (mdctx == NULL)
        return rc;

    if (EVP_DigestInit_ex(mdctx, md, NULL) != 1)
        goto out;

    if (EVP_DigestUpdate(mdctx, data, len) != 1)
        goto out;

    if (EVP_DigestFinal_ex(mdctx, digest, NULL) != 1)
        goto out;

    *digest_len = EVP_MD_size(md);
    rc = 0;
out:
    EVP_MD_CTX_destroy(mdctx);
    return rc;
}

int attest_util_decode_data(size_t input_len, const char *input, int offset,
                size_t *output_len, unsigned char **output)
{
    unsigned char *buf , *buf_ptr;
    EVP_ENCODE_CTX *ctx = NULL;
    int rc = -ENOMEM, decoded_len;

    input_len -= offset;
    input += offset;

    buf_ptr = buf = malloc(input_len / 4 * 3);
    if (!buf_ptr)
        return rc;

    ctx = EVP_ENCODE_CTX_new();
    if (!ctx)
        goto out;

    EVP_DecodeInit(ctx);

    rc = EVP_DecodeUpdate(ctx, buf, &decoded_len,
                  (unsigned char *)input, input_len);
    if (rc == -1) {
        rc = -EINVAL;
        goto out;
    }

    buf_ptr += decoded_len;

    if (rc == 1) {
        rc = EVP_DecodeFinal(ctx, buf_ptr, &decoded_len);
        if (rc == -1) {
            rc = -EINVAL;
            goto out;
        }

        buf_ptr += decoded_len;
        rc = 0;
    }

    *output_len = buf_ptr - buf;
    *output = buf;
out:
    if (rc)
        free(buf);

    EVP_ENCODE_CTX_free(ctx);
    return rc;
}

int attest_util_encode_data(size_t input_len, const unsigned char *input,
                int offset, size_t *output_len, char **output)
{
    EVP_ENCODE_CTX *ctx = NULL;
    int nr_blocks = input_len / DECODED_BLOCK_SIZE + 1, encoded_len = 0;
    unsigned char *buf, *buf_ptr;
    int rc = -ENOMEM, i, cur_len;

    buf_ptr = buf = malloc(offset + nr_blocks * ENCODED_BLOCK_SIZE + 1);
    if (!buf_ptr)
        return rc;

    buf_ptr += offset;

    ctx = EVP_ENCODE_CTX_new();
    if (!ctx) {
        printf("Out of memory\n");
        goto out;
    }

    EVP_EncodeInit(ctx);

    for (i = 0; i < nr_blocks; i++) {
        cur_len = input_len < DECODED_BLOCK_SIZE ?
              input_len : DECODED_BLOCK_SIZE;

        EVP_EncodeUpdate(ctx, buf_ptr, &encoded_len,
                 (unsigned char *)input, cur_len);
        if (i == DECODED_BLOCK_SIZE && !encoded_len) {
            rc = -EINVAL;
            goto out;
        }

        buf_ptr += encoded_len;
        input_len -= cur_len;
        input += cur_len;
    }

    EVP_EncodeFinal(ctx, buf_ptr, &encoded_len);
    buf_ptr += encoded_len;

    *output_len = buf_ptr - buf;
    *output = (char *)buf;
    *(*output + *output_len) = '\0';
    rc = 0;
out:
    if (rc)
        free(buf);

    EVP_ENCODE_CTX_free(ctx);
    return rc;
}

int attest_util_download_data(const char *url, int fd)
{
    CURL *curl;
    FILE *f;
    int rc;

    f = fdopen(fd, "wb");
    if (!f)
        return -EIO;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    curl = curl_easy_init();
    if (!curl)
        goto out_close;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)f);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    rc = curl_easy_perform(curl);
    if(rc != CURLE_OK) {
        printf("curl_easy_perform() failed: %s\n",
               curl_easy_strerror(rc));
        rc = -EACCES;
        goto out_cleanup;
    }

    fclose(f);
    f = NULL;
out_cleanup:
    curl_easy_cleanup(curl);
out_close:
    curl_global_cleanup();

    if (f)
        fclose(f);

    return rc;
}

int attest_util_check_mask(int mask_in_len, uint8_t *mask_in,
               int mask_ref_len, uint8_t *mask_ref)
{
    int i;

    if (mask_in_len > mask_ref_len)
        return -EINVAL;

    for (i = 0; i < mask_ref_len; i++) {
        if (i > mask_in_len && mask_ref[i])
            return -ENOENT;

        if ((mask_in[i] & mask_ref[i]) != mask_ref[i])
            return -ENOENT;
    }

    return 0;
}

int attest_util_parse_pcr_list(const char *pcr_list_str, int pcr_list_num,
                   int *pcr_list)
{
    char *list, *list_ptr, *pcr_str;
    int rc = 0, i, pcr;

    for (i = 0; i < pcr_list_num; i++)
        pcr_list[i] = -1;

    i = 0;

    list_ptr = list = strdup(pcr_list_str);
    if (!list)
        return -ENOMEM;

    while ((pcr_str = strsep(&list_ptr, ","))) {
        if (i > pcr_list_num) {
            rc = -ERANGE;
            goto out;
        }

        pcr = strtol(pcr_str, NULL, 10);
        if (pcr < 0) {
            rc = pcr;
            goto out;
        }

        pcr_list[i++] = pcr;
    }
out:
    free(list);
    return rc;
}

/**
 * @name Kernel Functions
 *  @{
 */

static const char hex_asc[] = "0123456789abcdef";

#define hex_asc_lo(x)	hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x)	hex_asc[((x) & 0xf0) >> 4]

static inline char *hex_byte_pack(char *buf, unsigned char byte)
{
    *buf++ = hex_asc_hi(byte);
    *buf++ = hex_asc_lo(byte);
    return buf;
}

/* from lib/hexdump.c (Linux kernel) */
static int hex_to_bin(char ch)
{
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    ch = tolower(ch);
    if ((ch >= 'a') && (ch <= 'f'))
        return ch - 'a' + 10;
    return -1;
}

int _hex2bin(unsigned char *dst, const char *src, size_t count)
{
    while (count--) {
        int hi = hex_to_bin(*src++);
        int lo = hex_to_bin(*src++);

        if ((hi < 0) || (lo < 0))
            return -1;

        *dst++ = (hi << 4) | lo;
    }
    return 0;
}

char *_bin2hex(char *dst, const void *src, size_t count)
{
    const unsigned char *_src = src;

    while (count--)
        dst = hex_byte_pack(dst, *_src++);
    return dst;
}
/** @}*/
/** @}*/
