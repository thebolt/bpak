/**
 * BPAK - Bit Packer
 *
 * Copyright (C) 2019 Jonas Blixt <jonpe960@gmail.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <bpak/bpak.h>
#include <bpak/pkg.h>
#include <bpak/utils.h>
#include <bpak/merkle.h>
#include <mbedtls/config.h>
#include <mbedtls/platform.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

static ssize_t merkle_wr(off_t offset,
                         uint8_t *buf,
                         size_t size,
                         void *priv)
{
    uint8_t *data = (uint8_t *) priv;
    memcpy(&data[offset], buf, size);
    return size;
}

static ssize_t merkle_rd(off_t offset,
                         uint8_t *buf,
                         size_t size,
                         void *priv)
{
    uint8_t *data = (uint8_t *) priv + offset;
    memcpy(buf, data, size);
    return size;
}

static void merkle_status(struct bpak_merkle_context *ctx)
{
    if ((ctx->current.byte_counter % (MERKLE_BLOCK_SZ)) == 0)
    {
        printf("\r %i: %li %%", ctx->current.level,
            100 * ctx->current.byte_counter / ctx->current.size);
        fflush(stdout);
    }
    else if(ctx->current.byte_counter == ctx->current.size)
    {
        printf("\r %i: 100 %%\n", ctx->current.level);
    }
}

int bpak_pkg_add_file_with_merkle_tree(struct bpak_package *pkg,
            const char *filename, const char *part_name, uint8_t flags)
{
    int rc;
    struct bpak_header *h = bpak_pkg_header(pkg);
    struct bpak_merkle_context ctx;
    struct stat statbuf;
    uint8_t block_buf[4096];
    uint8_t buffer2[4096];
    size_t chunk_sz;
    uint64_t new_offset = sizeof(*h);

    if (stat(filename, &statbuf) != 0) {
        bpak_printf(0, "Error: Can't open file '%s'\n", filename);
        return -BPAK_FILE_NOT_FOUND;
    }

    rc = bpak_pkg_add_file(pkg, filename, part_name, flags);

    if (rc != 0)
        return rc;

    size_t merkle_sz = bpak_merkle_compute_size(statbuf.st_size, -1, true);
    char *merkle_buf = malloc(merkle_sz);

    memset(merkle_buf, 0, merkle_sz);

    bpak_printf(1, "Allocated %li bytes for merkle tree\n", merkle_sz);

    bpak_merkle_hash_t salt;
    memset(salt, 0, 32);

    uint32_t *salt_ptr = (uint32_t *) salt;

    for (int i = 0; i < sizeof(salt)/sizeof(uint32_t); i++) {
        (*salt_ptr) = random() & 0xFFFFFFFF;
        salt_ptr++;
    }

    rc = bpak_merkle_init(&ctx, buffer2, 4096, statbuf.st_size, salt,
                            merkle_wr, merkle_rd, merkle_buf);

    for (int i = 0; i < ctx.no_of_levels; i++)
        bpak_printf(1, "Level %i size %li bytes\n", i,
                bpak_merkle_compute_size(statbuf.st_size, i, false));

    bpak_merkle_set_status_cb(&ctx, merkle_status);

    FILE *fp = fopen(filename, "rb");

    if (fp == NULL) {
        rc = -BPAK_FILE_NOT_FOUND;
        goto err_free_buf_out;
    }

    rc = BPAK_OK;
    while(true) {
        chunk_sz = fread(block_buf, 1, sizeof(block_buf), fp);

        if (chunk_sz == 0)
            break;

        rc = bpak_merkle_process(&ctx, block_buf, chunk_sz);

        if (rc != BPAK_OK)
            break;

    }

    if (rc != BPAK_OK)
        goto err_close_fp_out;

    while (!bpak_merkle_done(&ctx)) {
        rc = bpak_merkle_process(&ctx, NULL, 0);

        if (rc != BPAK_OK)
            break;
    }

    if (rc != BPAK_OK)
        goto err_close_fp_out;

    bpak_merkle_hash_t hash;

    rc = bpak_merkle_out(&ctx, hash);

    /* Write tree to bpak file */

    bpak_foreach_part(h, p) {
        new_offset += (p->size + p->pad_bytes);
    }

    /* Add salt */
    uint8_t *m = NULL;
    char tmp[512];

    rc = bpak_add_meta(h, bpak_id("merkle-salt"), bpak_id(part_name), (void **) &m,
                            sizeof(bpak_merkle_hash_t));

    if (rc != BPAK_OK)
        goto err_close_fp_out;

    memcpy(m, salt, sizeof(bpak_merkle_hash_t));

    m = NULL;
    rc = bpak_add_meta(h, bpak_id("merkle-root-hash"), bpak_id(part_name),
                        (void **) &m, sizeof(bpak_merkle_hash_t));

    if (rc != BPAK_OK)
        goto err_close_fp_out;

    memcpy(m, hash, sizeof(bpak_merkle_hash_t));

    struct bpak_part_header *p = NULL;

    snprintf(tmp, sizeof(tmp), "%s-hash-tree", part_name);
    rc = bpak_add_part(h, bpak_id(tmp), &p);

    if (rc != BPAK_OK) {
        bpak_printf(0, "Error: Could not add part\n");
        goto err_close_fp_out;
    }

    p->offset = new_offset;
    p->flags = flags;
    p->size = merkle_sz;
    p->pad_bytes = 0; /* Merkle tree is multiples of 4kByte, no padding needed */

    rc = fseek(pkg->fp, new_offset, SEEK_SET);

    if (rc != BPAK_OK) {
        bpak_printf(0, "Could not seek to new pos\n");
        goto err_close_fp_out;
    }

    if (fwrite(merkle_buf, 1, merkle_sz, pkg->fp) != merkle_sz) {
        rc = -BPAK_WRITE_ERROR;
        goto err_close_fp_out;
    }

    if (bpak_pkg_update_payload_hash(pkg) != BPAK_OK) {
        bpak_printf(0, "Error: Could not update payload hash\n");
        rc = -BPAK_FAILED;
        goto err_close_fp_out;
    }

    rc = bpak_pkg_write_header(pkg);
err_close_fp_out:
    fclose(fp);
err_free_buf_out:
    free(merkle_buf);
    return rc;
}

int bpak_pkg_add_file(struct bpak_package *pkg, const char *filename,
                     const char *part_name, uint8_t flags)
{
    int rc;
    struct bpak_header *h = bpak_pkg_header(pkg);
    struct bpak_part_header *p = NULL;
    struct stat statbuf;
    uint64_t new_offset = sizeof(struct bpak_header);

    if (stat(filename, &statbuf) != 0) {
        bpak_printf(0, "Error: can't open file '%s'\n", filename);
        return -BPAK_FAILED;
    }

    FILE *in_fp = NULL;

    bpak_printf(1, "Adding %s <%s>\n", part_name, filename);

    bpak_foreach_part(h, p) {
        new_offset += (p->size + p->pad_bytes);
    }

    rc = bpak_add_part(h, bpak_id(part_name), &p);

    if (rc != BPAK_OK) {
        bpak_printf(0, "Error: Could not add part\n");
        return rc;
    }

    p->id = bpak_id(part_name);
    p->offset = new_offset;
    p->flags = flags;
    p->size = statbuf.st_size;

    if (statbuf.st_size % BPAK_PART_ALIGN)
        p->pad_bytes = BPAK_PART_ALIGN - (statbuf.st_size % BPAK_PART_ALIGN);
    else
        p->pad_bytes = 0;

    rc = fseek(pkg->fp, new_offset, SEEK_SET);

    if (rc != 0) {
        bpak_printf(0, "Error: Could not seek to new pos\n");
        return -BPAK_SEEK_ERROR;
    }

    uint64_t bytes_to_write = p->size;

    in_fp = fopen(filename, "r");

    if (!in_fp) {
        bpak_printf(0, "Could not open input file: %s\n", filename);
        return -BPAK_FAILED;
    }

    char chunk_buffer[512];

    while (bytes_to_write) {
        size_t read_bytes = fread(chunk_buffer, 1, sizeof(chunk_buffer), in_fp);
        if (fwrite(chunk_buffer, 1, read_bytes, pkg->fp) != read_bytes) {
            rc = -BPAK_WRITE_ERROR;
            break;
        }
        bytes_to_write -= read_bytes;
    }

    if (rc != BPAK_OK) {
        goto err_close_fp;
    }

    if (p->pad_bytes) {
        bpak_printf(2, "Adding %i z-pad\n", p->pad_bytes);
        memset(chunk_buffer, 0, sizeof(chunk_buffer));
        if (fwrite(chunk_buffer, 1, p->pad_bytes, pkg->fp) != p->pad_bytes) {
            rc = -BPAK_WRITE_ERROR;
            goto err_close_fp;
        }
    }

    if (bpak_pkg_update_payload_hash(pkg) != BPAK_OK) {
        bpak_printf(0, "Error: Could not update payload hash\n");
        rc = -BPAK_FAILED;
        goto err_close_fp;
    }

    rc = bpak_pkg_write_header(pkg);

err_close_fp:
    fclose(in_fp);
    return rc;
}

int bpak_pkg_add_key(struct bpak_package *pkg, const char *filename,
                     const char *part_name, uint8_t flags)
{
    int rc;
    char tmp[4096];
    struct bpak_header *h = bpak_pkg_header(pkg);
    struct bpak_part_header *p = NULL;
    struct stat statbuf;
    uint64_t new_offset = sizeof(struct bpak_header);
    mbedtls_pk_context ctx;
    mbedtls_pk_init(&ctx);
    mbedtls_pk_parse_public_keyfile(&ctx, filename);

    int len = mbedtls_pk_write_pubkey_der(&ctx, tmp, sizeof(tmp));

    if (len < 0) {
        bpak_printf(0, "Error: Could not load public key '%s'\n", filename);
        rc = -BPAK_FAILED;
        return rc;
    }

    bpak_printf(1, "Loaded public key %i bytes\n", len);

    mbedtls_pk_free(&ctx);

    /* Write header */
    bpak_foreach_part(h, p) {
        new_offset += (p->size + p->pad_bytes);
    }

    rc = bpak_add_part(h, bpak_id(part_name), &p);

    if (rc != BPAK_OK) {
        bpak_printf(0, "Error: Could not add part\n");
        return rc;
    }

    p->id = bpak_id(part_name);
    p->offset = new_offset;
    p->flags = 0;
    p->size = len;

    if (len % BPAK_PART_ALIGN)
        p->pad_bytes = BPAK_PART_ALIGN - (len % BPAK_PART_ALIGN);
    else
        p->pad_bytes = 0;

    rc = fseek(pkg->fp, new_offset, SEEK_SET);

    if (rc != 0) {
        bpak_printf(0, "Could not seek to new pos\n");
        return -BPAK_SEEK_ERROR;
    }

    char chunk_buffer[512];
    uint64_t bytes_to_write = p->size;
    uint64_t bytes_offset = 0;
    uint64_t chunk_sz = sizeof(chunk_buffer);

    while (bytes_to_write) {
        chunk_sz = (bytes_to_write > sizeof(chunk_buffer)) ? \
                            sizeof(chunk_buffer):bytes_to_write;

        memcpy(chunk_buffer, &tmp[sizeof(tmp) - len + bytes_offset], chunk_sz);

        if (fwrite(chunk_buffer, 1, chunk_sz, pkg->fp) != chunk_sz) {
            rc = -BPAK_WRITE_ERROR;
            break;
        }
        bytes_to_write -= chunk_sz;
        bytes_offset += chunk_sz;
    }

    if (rc != BPAK_OK)
        return rc;

    memset(chunk_buffer, 0, sizeof(chunk_buffer));
    if (fwrite(chunk_buffer, 1, p->pad_bytes, pkg->fp) != p->pad_bytes) {
        return -BPAK_WRITE_ERROR;
    }


    if (bpak_pkg_update_payload_hash(pkg) != BPAK_OK) {
        bpak_printf(0, "Error: Could not update payload hash\n");
        return -BPAK_FAILED;
    }

    return bpak_pkg_write_header(pkg);
}
