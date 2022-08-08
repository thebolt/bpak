#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <bpak/bpak.h>
#include <bpak/crc.h>
#include <bpak/pkg.h>
#include <bpak/file.h>
#include <bpak/utils.h>
#include <bpak/transport.h>

#include "sha256.h"
#include "sha512.h"

int bpak_pkg_open(struct bpak_package **pkg_, const char *filename,
                  const char *mode)
{
    int rc;

    if (!mode)
        return -BPAK_FAILED;

    bpak_printf(1, "Opening BPAK file %s\n", filename);

    *pkg_ = malloc(sizeof(struct bpak_package));
    struct bpak_package *pkg = *pkg_;

    if (!*pkg_)
        return -BPAK_FAILED;

    memset(pkg, 0, sizeof(*pkg));

    rc = bpak_io_init_file(&pkg->io, filename, mode);

    if (rc != BPAK_OK)
        goto err_free_pkg;

    pkg->header_location = BPAK_HEADER_POS_FIRST;
    rc = bpak_io_seek(pkg->io, 0, BPAK_IO_SEEK_SET);

    if (rc != BPAK_OK)
        goto err_close_io;

    size_t read_bytes = bpak_io_read(pkg->io, &pkg->header,
                                        sizeof(pkg->header));
    if (read_bytes != sizeof(pkg->header))
    {
        rc = -BPAK_FAILED;
        goto skip_header;
    }

    rc = bpak_valid_header(&pkg->header);

    if (rc != BPAK_OK) {
        /* Check if the header is at the end */
        rc = bpak_io_seek(pkg->io, 4096, BPAK_IO_SEEK_END);

        if (rc != BPAK_OK) {
            goto skip_header;
        }

        read_bytes = bpak_io_read(pkg->io, &pkg->header,
                                            sizeof(pkg->header));
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
    rc = bpak_io_seek(pkg->io, 0, BPAK_IO_SEEK_SET);

    if (rc != BPAK_OK)
        goto err_close_io;

    return BPAK_OK;

err_close_io:
    bpak_io_close(pkg->io);
err_free_pkg:
    free(pkg);
    *pkg_ = NULL;
    return rc;
}

int bpak_pkg_close(struct bpak_package *pkg)
{
    bpak_io_close(pkg->io);
    free(pkg);
    return BPAK_OK;
}

int bpak_pkg_update_payload_hash(struct bpak_package *pkg)
{
    size_t payload_size = sizeof(pkg->header.payload_hash);

    return bpak_pkg_compute_payload_hash(pkg, (char *) pkg->header.payload_hash,
                                                            &payload_size);
}

int bpak_pkg_compute_header_hash(struct bpak_package *pkg, char *output,
                                 size_t *size, bool update_payload_hash)
{
    int rc;
    uint8_t signature[512];
    uint16_t signature_sz;
    mbedtls_sha256_context sha256;
    mbedtls_sha512_context sha512;

    if (update_payload_hash) {
        size_t payload_size = sizeof(pkg->header.payload_hash);

        rc = bpak_pkg_compute_payload_hash(pkg, (char *) pkg->header.payload_hash,
                                            &payload_size);

        if (rc != BPAK_OK) {
            fprintf(stderr, "Error: Failed to compute payload hash\n");
            return rc;
        }
    }

    memcpy(signature, pkg->header.signature, sizeof(signature));
    signature_sz = pkg->header.signature_sz;

    memset(pkg->header.signature, 0, sizeof(pkg->header.signature));
    pkg->header.signature_sz = 0;

    switch (pkg->header.hash_kind)
    {
        case BPAK_HASH_SHA256:
            if (*size < 32)
                return -BPAK_FAILED;
            *size = 32;
            mbedtls_sha256_init(&sha256);
            mbedtls_sha256_starts_ret(&sha256, 0);
        break;
        case BPAK_HASH_SHA384:
            if (*size < 48)
                return -BPAK_FAILED;
            *size = 48;
            mbedtls_sha512_init(&sha512);
            mbedtls_sha512_starts_ret(&sha512, 1);
        break;
        case BPAK_HASH_SHA512:
            if (*size < 64)
                return -BPAK_FAILED;
            *size = 64;
            mbedtls_sha512_init(&sha512);
            mbedtls_sha512_starts_ret(&sha512, 0);
        break;
        default:
            return -BPAK_NOT_SUPPORTED;
    }
    if (pkg->header.hash_kind == BPAK_HASH_SHA256)
        rc = mbedtls_sha256_update_ret(&sha256, (const unsigned char *) &pkg->header,
                                        sizeof(pkg->header));
    else
        rc = mbedtls_sha512_update_ret(&sha512, (const unsigned char *) &pkg->header,
                                        sizeof(pkg->header));

    if (pkg->header.hash_kind == BPAK_HASH_SHA256)
        mbedtls_sha256_finish_ret(&sha256, (unsigned char *) output);
    else
        mbedtls_sha512_finish_ret(&sha512, (unsigned char*) output);

    memcpy(pkg->header.signature, signature, sizeof(signature));
    pkg->header.signature_sz = signature_sz;
    return BPAK_OK;
}

int bpak_pkg_compute_payload_hash(struct bpak_package *pkg, char *output,
                                 size_t *size)
{
    int rc;
    mbedtls_sha256_context sha256;
    mbedtls_sha512_context sha512;

    switch (pkg->header.hash_kind)
    {
        case BPAK_HASH_SHA256:
            if (*size < 32)
                return -BPAK_FAILED;
            *size = 32;
            mbedtls_sha256_init(&sha256);
            mbedtls_sha256_starts_ret(&sha256, 0);
        break;
        case BPAK_HASH_SHA384:
            if (*size < 48)
                return -BPAK_FAILED;
            *size = 48;
            mbedtls_sha512_init(&sha512);
            mbedtls_sha512_starts_ret(&sha512, 1);
        break;
        case BPAK_HASH_SHA512:
            if (*size < 64)
                return -BPAK_FAILED;
            *size = 64;
            mbedtls_sha512_init(&sha512);
            mbedtls_sha512_starts_ret(&sha512, 0);
        break;
        default:
            return -BPAK_NOT_SUPPORTED;
    }

    char hash_buffer[512];

    if (pkg->header_location == BPAK_HEADER_POS_FIRST) {
        bpak_io_seek(pkg->io, sizeof(pkg->header), BPAK_IO_SEEK_SET);
    } else {
        bpak_io_seek(pkg->io, 0, BPAK_IO_SEEK_SET);
    }

    bpak_foreach_part(&pkg->header, p) {
        size_t bytes_to_read = bpak_part_size(p);
        size_t chunk = 0;

        if (!p->id)
            continue;

        if (p->flags & BPAK_FLAG_EXCLUDE_FROM_HASH)
        {
            bpak_io_seek(pkg->io, bpak_part_size(p), BPAK_IO_SEEK_FWD);
            continue;
        }

        do
        {
            chunk = (bytes_to_read > sizeof(hash_buffer))?
                        sizeof(hash_buffer):bytes_to_read;

            bpak_io_read(pkg->io, hash_buffer, chunk);

            if (pkg->header.hash_kind == BPAK_HASH_SHA256)
                rc = mbedtls_sha256_update_ret(&sha256, hash_buffer, chunk);
            else
                rc = mbedtls_sha512_update_ret(&sha512, hash_buffer, chunk);

            bytes_to_read -= chunk;
        } while (bytes_to_read);
    }

    if (pkg->header.hash_kind == BPAK_HASH_SHA256)
        mbedtls_sha256_finish_ret(&sha256, output);
    else
        mbedtls_sha512_finish_ret(&sha512, output);

    return BPAK_OK;
}

size_t bpak_pkg_installed_size(struct bpak_package *pkg)
{
    size_t installed_size = 0;

    bpak_foreach_part(&pkg->header, p)
    {
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
    int rc;

    if (pkg->header_location == BPAK_HEADER_POS_FIRST) {
        rc = bpak_io_seek(pkg->io, 0, BPAK_IO_SEEK_SET);
    } else {
        rc = bpak_io_seek(pkg->io, 4096, BPAK_IO_SEEK_END);
    }

    if (rc != BPAK_OK) {
        bpak_printf(0, "%s: Could not seek\n", __func__);
        return rc;
    }

    rc = bpak_io_write(pkg->io, &pkg->header, sizeof(pkg->header));

    if (rc != sizeof(pkg->header)) {
        bpak_printf(0, "%s: Write failed\n", __func__);
        return -BPAK_FAILED;
    }

    return BPAK_OK;
}

int bpak_pkg_sign(struct bpak_package *pkg, const uint8_t *signature,
                    size_t size)
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
    struct bpak_io *output_io;
    struct bpak_io *origin_io;
    off_t origin_offset;
    off_t output_offset;
};

static ssize_t decode_write_output(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (bpak_io_seek(priv->output_io, priv->output_offset + offset,
                        BPAK_IO_SEEK_SET) != BPAK_OK) {
        return -BPAK_SEEK_ERROR;
    }


    return bpak_io_write(priv->output_io, buffer, length);
}

static ssize_t decode_read_output(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (bpak_io_seek(priv->output_io, priv->output_offset + offset,
                        BPAK_IO_SEEK_SET) != BPAK_OK) {
        return -BPAK_SEEK_ERROR;
    }

    return bpak_io_read(priv->output_io, buffer, length);
}

static ssize_t decode_write_output_header(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (length != sizeof(struct bpak_header))
        return -BPAK_SIZE_ERROR;

    if (bpak_io_seek(priv->output_io, 0,
                        BPAK_IO_SEEK_SET) != BPAK_OK) {
        return -BPAK_SEEK_ERROR;
    }

    return bpak_io_write(priv->output_io, buffer, length);
}

static ssize_t decode_read_origin(off_t offset,
                             uint8_t *buffer,
                             size_t length,
                             void *user)
{
    struct decode_private *priv = (struct decode_private *) user;

    if (bpak_io_seek(priv->origin_io, priv->origin_offset + offset,
                        BPAK_IO_SEEK_SET) != BPAK_OK) {
        return -BPAK_SEEK_ERROR;
    }

    return bpak_io_read(priv->origin_io, buffer, length);
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
    decode_private.output_io = output->io;
    if (origin != NULL)
        decode_private.origin_io = origin->io;
    else
        decode_private.origin_io = NULL;

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

    rc = bpak_io_seek(input->io,
                 sizeof(struct bpak_header),
                 BPAK_IO_SEEK_SET);

    if (rc != BPAK_OK) {
        bpak_printf(0, "%s: Error, could not seek input stream", __func__);
        return rc;
    }

    bpak_foreach_part(patch_header, part) {
        if (part->id == 0)
            break;

        /* Compute origin and output offsets */
        if (origin->io != NULL) {
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
            size_t bytes_read = bpak_io_read(input->io, chunk_buffer, chunk_length);

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
    return bpak_transport_encode(input, output, origin);
}
