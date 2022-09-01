/**
 * BPAK - Bit Packer
 *
 * Copyright (C) 2022 Jonas Blixt <jonpe960@gmail.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <bpak/bpak.h>
#include <bpak/verify.h>
#include <bpak/crc.h>
#include <bpak/pkg.h>
#include <bpak/utils.h>
#include <bpak/transport.h>
#include <mbedtls/config.h>
#include <mbedtls/platform.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

int bpak_pkg_open(struct bpak_package *pkg, const char *filename,
                  const char *mode)
{
    int rc;

    if (!mode)
        return -BPAK_FAILED;

    bpak_printf(1, "Opening BPAK file %s\n", filename);

    memset(pkg, 0, sizeof(*pkg));

    pkg->fp = fopen(filename, mode);

    if (pkg->fp == NULL)
        return -BPAK_NOT_FOUND;

    pkg->header_location = BPAK_HEADER_POS_FIRST;

    if (fseek(pkg->fp, 0, SEEK_SET) != 0) {
        rc = -BPAK_SEEK_ERROR;
        goto err_close_io;
    }

    size_t read_bytes = fread(&pkg->header, 1, sizeof(pkg->header), pkg->fp);

    if (read_bytes != sizeof(pkg->header))
    {
        rc = -BPAK_READ_ERROR;
        goto skip_header;
    }

    rc = bpak_valid_header(&pkg->header);

    if (rc != BPAK_OK) {
        /* Check if the header is at the end */

        if (fseek(pkg->fp, 4096, SEEK_END) != 0) {
            rc = -BPAK_SEEK_ERROR;
            goto skip_header;
        }

        read_bytes = fread(&pkg->header, 1, sizeof(pkg->header), pkg->fp);

        if (read_bytes != sizeof(pkg->header))
        {
            rc = -BPAK_FAILED;
            goto skip_header;
        }

        rc = bpak_valid_header(&pkg->header);

        if (rc != BPAK_OK)
            goto skip_header;

        pkg->header_location = BPAK_HEADER_POS_LAST;
    }

skip_header:
    if (fseek(pkg->fp, 0, SEEK_SET) != 0) {
        rc = -BPAK_SEEK_ERROR;
        goto err_close_io;
    }

    return BPAK_OK;

err_close_io:
    fclose(pkg->fp);
    return rc;
}

int bpak_pkg_close(struct bpak_package *pkg)
{
    if (pkg->fp != NULL) {
        fclose(pkg->fp);
        pkg->fp = NULL;
    }
    return BPAK_OK;
}

static ssize_t pkg_read_payload(off_t offset, uint8_t *buf, size_t length,
                                void *user)
{
    FILE *fp = (FILE *) user;
    if (fseek(fp, offset, SEEK_SET) != 0)
        return -BPAK_SEEK_ERROR;
    size_t read_bytes = fread(buf, 1, length, fp);

    if (read_bytes != length)
        return -BPAK_READ_ERROR;

    return read_bytes;
}

int bpak_pkg_update_hash(struct bpak_package *pkg, char *output, size_t *size)
{
    int rc;

    size_t hash_size = sizeof(pkg->header.payload_hash);

    rc = bpak_verify_compute_payload_hash(&pkg->header,
                                          pkg_read_payload,
                                          sizeof(struct bpak_header),
                                          (void *) pkg->fp,
                                          pkg->header.payload_hash,
                                          &hash_size);

    if (rc != BPAK_OK)
        return rc;

    if ((output) != 0 && (size != NULL)) {
        rc = bpak_verify_compute_header_hash(&pkg->header, output, size);
        if (rc != BPAK_OK)
            return rc;
    }

    return BPAK_OK;
}

size_t bpak_pkg_installed_size(struct bpak_package *pkg)
{
    size_t installed_size = 0;

    bpak_foreach_part(&pkg->header, p) {
        installed_size += p->size + p->pad_bytes;
    }

    return installed_size;
}

size_t bpak_pkg_size(struct bpak_package *pkg)
{
    size_t transport_size = 0;

    bpak_foreach_part(&pkg->header, p)
    {
        if (p->flags & BPAK_FLAG_TRANSPORT)
            transport_size += p->transport_size;
        else
            transport_size += p->size;
    }

    transport_size += sizeof(struct bpak_header);

    return transport_size;
}

struct bpak_header *bpak_pkg_header(struct bpak_package *pkg)
{
    return &pkg->header;
}

int bpak_pkg_write_header(struct bpak_package *pkg)
{
    if (pkg->header_location == BPAK_HEADER_POS_FIRST) {
        if (fseek(pkg->fp, 0, SEEK_SET) != 0) {
            return -BPAK_SEEK_ERROR;
        }
    } else {
        if (fseek(pkg->fp, sizeof(struct bpak_header), SEEK_END) != 0) {
            return -BPAK_SEEK_ERROR;
        }
    }

    size_t bytes_written = fwrite(&pkg->header, 1, sizeof(pkg->header), pkg->fp);

    if (bytes_written != sizeof(pkg->header)) {
        bpak_printf(0, "%s: Write failed\n", __func__);
        return -BPAK_WRITE_ERROR;
    }

    return BPAK_OK;
}

int bpak_pkg_write_raw_signature(struct bpak_package *pkg,
                                 const uint8_t *signature, size_t size)
{
    int rc;
    uint8_t *signature_ptr = NULL;

    memset(pkg->header.signature, 0, sizeof(pkg->header.signature));
    memcpy(pkg->header.signature, signature, size);
    pkg->header.signature_sz = size;

    return bpak_pkg_write_header(pkg);
}

struct decode_private
{
    FILE *output_fp;
    FILE *origin_fp;
    off_t origin_offset;
    off_t output_offset;
};

static ssize_t decode_write_output(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (fseek(priv->output_fp, priv->output_offset + offset, SEEK_SET) != 0) {
        return -BPAK_SEEK_ERROR;
    }

    return fwrite(buffer, 1, length, priv->output_fp);
}

static ssize_t decode_read_output(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (fseek(priv->output_fp, priv->output_offset + offset, SEEK_SET) != 0) {
        return -BPAK_SEEK_ERROR;
    }

    return fread(buffer, 1, length, priv->output_fp);
}

static ssize_t decode_write_output_header(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (length != sizeof(struct bpak_header))
        return -BPAK_SIZE_ERROR;

    if (fseek(priv->output_fp, 0, SEEK_SET) != 0) {
        return -BPAK_SEEK_ERROR;
    }

    return fwrite(buffer, 1, length, priv->output_fp);
}

static ssize_t decode_read_origin(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (fseek(priv->origin_fp, priv->origin_offset + offset, SEEK_SET) != 0) {
        return -BPAK_SEEK_ERROR;
    }

    return fread(buffer, 1, length, priv->origin_fp);
}

int bpak_pkg_transport_decode(struct bpak_package *input,
                              struct bpak_package *output,
                              struct bpak_package *origin)
{
    int rc;

    struct bpak_header *patch_header = bpak_pkg_header(input);
    struct bpak_part_header *part = NULL;
    struct bpak_part_header *origin_part = NULL;
    struct bpak_transport_decode decode_ctx;
    uint8_t decode_buffer[4096];
    uint8_t chunk_buffer[4096];
    uint8_t decode_context_buffer[1024];
    ssize_t chunk_length;
    struct decode_private decode_private;

    memset(&decode_private, 0, sizeof(struct decode_private));
    decode_private.output_fp = output->fp;
    if (origin != NULL)
        decode_private.origin_fp = origin->fp;
    else
        decode_private.origin_fp = NULL;

    rc = bpak_transport_decode_init(&decode_ctx,
                                    decode_buffer,
                                    sizeof(decode_buffer),
                                    decode_context_buffer,
                                    sizeof(decode_context_buffer),
                                    patch_header,
                                    decode_write_output,
                                    decode_read_output,
                                    decode_write_output_header,
                                    &decode_private);

    if (rc != BPAK_OK) {
        bpak_printf(0, "%s: Error: Transport decode init failed (%i) %s", __func__,
                rc, bpak_error_string(rc));
        goto err_out;
    }

    if (origin != NULL) {
        struct bpak_header *origin_header = bpak_pkg_header(origin);

        rc = bpak_transport_decode_set_origin(&decode_ctx,
                                              origin_header,
                                              decode_read_origin);

        if (rc != BPAK_OK) {
            bpak_printf(0, "Error: Origin stream init failed (%i) %s\n",
                rc, bpak_error_string(rc));
            goto err_out;
        }
    }


    if (fseek(input->fp, sizeof(struct bpak_header), SEEK_SET) != 0) {
        bpak_printf(0, "%s: Error, could not seek input stream", __func__);
        return -BPAK_SEEK_ERROR;
    }

    bpak_foreach_part(patch_header, part) {
        if (part->id == 0)
            break;

        /* Compute origin and output offsets */
        if (origin->fp != NULL) {
            rc = bpak_get_part(&origin->header, part->id, &origin_part);

            if (rc != BPAK_OK) {
                bpak_printf(0, "Error could not get part with ref %x\n", part->id);
                goto err_out;
            }

            decode_private.origin_offset = bpak_part_offset(&origin->header,
                                                            origin_part);
        }

        decode_private.output_offset = bpak_part_offset(patch_header, part);

        rc = bpak_transport_decode_start(&decode_ctx, part);

        if (rc != BPAK_OK) {
            bpak_printf(0,"Error: Decoder start failed for part 0x%x (%i)\n",
                                part->id, rc);
            goto err_out;
        }

        /* If there is any input data chunk it up and feed the decoder */
        size_t bytes_to_process = bpak_part_size(part);

        while (bytes_to_process) {
            size_t chunk_length = BPAK_MIN(bytes_to_process, sizeof(chunk_buffer));
            size_t bytes_read = fread(chunk_buffer, 1, chunk_length, input->fp);

            if (bytes_read != chunk_length) {
                bpak_printf(0, "%s: bytes_read != chunk_length\n", __func__);
                return -BPAK_READ_ERROR;
            }

            rc = bpak_transport_decode_write_chunk(&decode_ctx, chunk_buffer,
                                                    chunk_length);

            if (rc != BPAK_OK) {
                bpak_printf(0,"Error: Decoder write chunk failed for part 0x%x (%i)\n",
                                    part->id, rc);
                goto err_out;
            }

            bytes_to_process -= chunk_length;
        }

        rc = bpak_transport_decode_finish(&decode_ctx);

        if (rc != BPAK_OK) {
            bpak_printf(0,"Error: Decoder finish failed for part 0x%x (%i)\n",
                                part->id, rc);
            goto err_out;
        }
    }

err_out:
    bpak_transport_decode_free(&decode_ctx);
    return rc;
}

int bpak_pkg_transport_encode(struct bpak_package *input,
                              struct bpak_package *output,
                              struct bpak_package *origin)
{
    FILE *origin_fp = NULL;
    struct bpak_header *origin_header = NULL;

    if (origin != NULL) {
        if (origin->fp != NULL) {
            origin_fp = origin->fp;
            origin_header = &origin->header;
        }
    }

    return bpak_transport_encode(input->fp, &input->header,
                                 output->fp, &output->header,
                                 origin_fp, origin_header);
}
