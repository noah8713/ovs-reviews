/* Copyright (c) 2009, 2010, 2011, 2012, 2013, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "openvswitch/vlog.h"
#include "lockfile.h"
#include "ovsdb.h"
#include "ovsdb-error.h"
#include "sha1.h"
#include "socket-util.h"
#include "transaction.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(ovsdb_log);

enum ovsdb_log_mode {
    OVSDB_LOG_READ,
    OVSDB_LOG_WRITE
};

struct ovsdb_log {
    off_t prev_offset;
    off_t offset;
    char *name;
    char *magic;
    struct lockfile *lockfile;
    FILE *stream;
    struct ovsdb_error *read_error;
    bool write_error;
    enum ovsdb_log_mode mode;
};

/* Attempts to open 'name' with the specified 'open_mode'.  On success, stores
 * the new log into '*filep' and returns NULL; otherwise returns NULL and
 * stores NULL into '*filep'.
 *
 * 'magic' is a short text string put at the beginning of every record and used
 * to distinguish one kind of log file from another.  For a conventional OVSDB
 * log file, use OVSDB_MAGIC.
 *
 * Whether the file will be locked using lockfile_lock() depends on 'locking':
 * use true to lock it, false not to lock it, or -1 to lock it only if
 * 'open_mode' is a mode that allows writing.
 */
struct ovsdb_error *
ovsdb_log_open(const char *name, const char *magic,
               enum ovsdb_log_open_mode open_mode,
               int locking, struct ovsdb_log **filep)
{
    struct lockfile *lockfile;
    struct ovsdb_error *error;
    struct ovsdb_log *file;
    struct stat s;
    FILE *stream;
    int flags;
    int fd;

    *filep = NULL;

    ovs_assert(locking == -1 || locking == false || locking == true);
    if (locking < 0) {
        locking = open_mode != OVSDB_LOG_READ_ONLY;
    }
    if (locking) {
        int retval = lockfile_lock(name, &lockfile);
        if (retval) {
            error = ovsdb_io_error(retval, "%s: failed to lock lockfile",
                                   name);
            goto error;
        }
    } else {
        lockfile = NULL;
    }

    switch (open_mode) {
    case OVSDB_LOG_READ_ONLY:
        flags = O_RDONLY;
        break;

    case OVSDB_LOG_READ_WRITE:
        flags = O_RDWR;
        break;

    case OVSDB_LOG_CREATE_EXCL:
        flags = O_RDWR | O_CREAT | O_EXCL;
        break;

    case OVSDB_LOG_CREATE:
        flags = O_RDWR | O_CREAT;
        break;

    default:
        OVS_NOT_REACHED();
    }
#ifdef _WIN32
    flags = flags | O_BINARY;
#endif
    /* Special case for /dev/stdin to make it work even if the operating system
     * doesn't support it under that name. */
    if (!strcmp(name, "/dev/stdin") && open_mode == OVSDB_LOG_READ_ONLY) {
        fd = dup(STDIN_FILENO);
    } else {
        fd = open(name, flags, 0666);
    }
    if (fd < 0) {
        const char *op = (open_mode == OVSDB_LOG_CREATE_EXCL ? "create"
            : open_mode == OVSDB_LOG_CREATE ? "create or open"
            : "open");
        error = ovsdb_io_error(errno, "%s: %s failed", name, op);
        goto error_unlock;
    }

    if (!fstat(fd, &s)) {
        if (s.st_size == 0) {
            /* It's (probably) a new file so fsync() its parent directory to
             * ensure that its directory entry is committed to disk. */
            fsync_parent_dir(name);
        } else if (s.st_size >= strlen(magic) && S_ISREG(s.st_mode)) {
            /* Try to read the magic from the first log record.  If it's not
             * the magic we expect, this is the wrong kind of file, so reject
             * it immediately. */
            size_t magic_len = strlen(magic);
            char *buf = xzalloc(magic_len + 1);
            bool err = (read(fd, buf, magic_len) == magic_len
                        && strcmp(buf, magic));
            free(buf);
            if (err) {
                error = ovsdb_error(NULL, "%s: bad magic (unexpected "
                                    "kind of file)", name);
                goto error_close;
            }
            if (lseek(fd, 0, SEEK_SET)) {
                error = ovsdb_io_error(errno, "%s: seek failed", name);
                goto error_close;
            }
        }
    }

    stream = fdopen(fd, open_mode == OVSDB_LOG_READ_ONLY ? "rb" : "w+b");
    if (!stream) {
        error = ovsdb_io_error(errno, "%s: fdopen failed", name);
        goto error_close;
    }

    file = xmalloc(sizeof *file);
    file->name = xstrdup(name);
    file->magic = xstrdup(magic);
    file->lockfile = lockfile;
    file->stream = stream;
    file->prev_offset = 0;
    file->offset = 0;
    file->read_error = NULL;
    file->write_error = false;
    file->mode = OVSDB_LOG_READ;
    *filep = file;
    return NULL;

error_close:
    close(fd);
error_unlock:
    lockfile_unlock(lockfile);
error:
    return error;
}

void
ovsdb_log_close(struct ovsdb_log *file)
{
    if (file) {
        free(file->name);
        if (file->stream) {
            fclose(file->stream);
        }
        lockfile_unlock(file->lockfile);
        ovsdb_error_destroy(file->read_error);
        free(file);
    }
}

static bool
parse_header(const char *magic, char *header, unsigned long int *length,
             uint8_t sha1[SHA1_DIGEST_SIZE])
{
    char *p;

    /* 'header' must consist of a magic string... */
    size_t magic_len = strlen(magic);
    if (strncmp(header, magic, magic_len) || header[magic_len] != ' ') {
        return false;
    }

    /* ...followed by a length in bytes... */
    *length = strtoul(header + magic_len + 1, &p, 10);
    if (!*length || *length == ULONG_MAX || *p != ' ') {
        return false;
    }
    p++;

    /* ...followed by a SHA-1 hash... */
    if (!sha1_from_hex(sha1, p)) {
        return false;
    }
    p += SHA1_HEX_DIGEST_LEN;

    /* ...and ended by a new-line. */
    if (*p != '\n') {
        return false;
    }

    return true;
}

static struct ovsdb_error *
parse_body(struct ovsdb_log *file, off_t offset, unsigned long int length,
           uint8_t sha1[SHA1_DIGEST_SIZE], struct json **jsonp)
{
    struct json_parser *parser;
    struct sha1_ctx ctx;

    sha1_init(&ctx);
    parser = json_parser_create(JSPF_TRAILER);

    while (length > 0) {
        char input[BUFSIZ];
        int chunk;

        chunk = MIN(length, sizeof input);
        if (fread(input, 1, chunk, file->stream) != chunk) {
            json_parser_abort(parser);
            return ovsdb_io_error(ferror(file->stream) ? errno : EOF,
                                  "%s: error reading %lu bytes "
                                  "starting at offset %lld", file->name,
                                  length, (long long int) offset);
        }
        sha1_update(&ctx, input, chunk);
        json_parser_feed(parser, input, chunk);
        length -= chunk;
    }

    sha1_final(&ctx, sha1);
    *jsonp = json_parser_finish(parser);
    return NULL;
}

struct ovsdb_error *
ovsdb_log_read(struct ovsdb_log *file, struct json **jsonp)
{
    uint8_t expected_sha1[SHA1_DIGEST_SIZE];
    uint8_t actual_sha1[SHA1_DIGEST_SIZE];
    struct ovsdb_error *error;
    off_t data_offset;
    unsigned long data_length;
    struct json *json;
    char header[128];

    *jsonp = json = NULL;

    if (file->read_error) {
        return ovsdb_error_clone(file->read_error);
    } else if (file->mode == OVSDB_LOG_WRITE) {
        return OVSDB_BUG("reading file in write mode");
    }

    if (!fgets(header, sizeof header, file->stream)) {
        if (feof(file->stream)) {
            error = NULL;
        } else {
            error = ovsdb_io_error(errno, "%s: read failed", file->name);
        }
        goto error;
    }

    if (!parse_header(file->magic, header, &data_length, expected_sha1)) {
        error = ovsdb_syntax_error(NULL, NULL, "%s: parse error at offset "
                                   "%lld in header line \"%.*s\"",
                                   file->name, (long long int) file->offset,
                                   (int) strcspn(header, "\n"), header);
        goto error;
    }

    data_offset = file->offset + strlen(header);
    error = parse_body(file, data_offset, data_length, actual_sha1, &json);
    if (error) {
        goto error;
    }

    if (memcmp(expected_sha1, actual_sha1, SHA1_DIGEST_SIZE)) {
        error = ovsdb_syntax_error(NULL, NULL, "%s: %lu bytes starting at "
                                   "offset %lld have SHA-1 hash "SHA1_FMT" "
                                   "but should have hash "SHA1_FMT,
                                   file->name, data_length,
                                   (long long int) data_offset,
                                   SHA1_ARGS(actual_sha1),
                                   SHA1_ARGS(expected_sha1));
        goto error;
    }

    if (json->type == JSON_STRING) {
        error = ovsdb_syntax_error(NULL, NULL, "%s: %lu bytes starting at "
                                   "offset %lld are not valid JSON (%s)",
                                   file->name, data_length,
                                   (long long int) data_offset,
                                   json->u.string);
        goto error;
    }
    if (json->type != JSON_OBJECT) {
        error = ovsdb_syntax_error(NULL, NULL, "%s: %lu bytes starting at "
                                   "offset %lld are not a JSON object",
                                   file->name, data_length,
                                   (long long int) data_offset);
        goto error;
    }

    file->prev_offset = file->offset;
    file->offset = data_offset + data_length;
    *jsonp = json;
    return NULL;

error:
    file->read_error = ovsdb_error_clone(error);
    json_destroy(json);
    return error;
}

/* Causes the log record read by the previous call to ovsdb_log_read() to be
 * effectively discarded.  The next call to ovsdb_log_write() will overwrite
 * that previously read record.
 *
 * Calling this function more than once has no additional effect.
 *
 * This function is useful when ovsdb_log_read() successfully reads a record
 * but that record does not make sense at a higher level (e.g. it specifies an
 * invalid transaction). */
void
ovsdb_log_unread(struct ovsdb_log *file)
{
    ovs_assert(file->mode == OVSDB_LOG_READ);
    file->offset = file->prev_offset;
}

void
ovsdb_log_compose_record(const struct json *json,
                         const char *magic, struct ds *header, struct ds *data)
{
    ovs_assert(json->type == JSON_OBJECT || json->type == JSON_ARRAY);
    ovs_assert(!header->length);
    ovs_assert(!data->length);

    /* Compose content.  Add a new-line (replacing the null terminator) to make
     * the file easier to read, even though it has no semantic value.  */
    json_to_ds(json, 0, data);
    ds_put_char(data, '\n');

    /* Compose header. */
    uint8_t sha1[SHA1_DIGEST_SIZE];
    sha1_bytes(data->string, data->length, sha1);
    ds_put_format(header, "%s %"PRIuSIZE" "SHA1_FMT"\n",
                  magic, data->length, SHA1_ARGS(sha1));
}

struct ovsdb_error *
ovsdb_log_write(struct ovsdb_log *file, const struct json *json)
{
    struct ovsdb_error *error;

    if (file->mode == OVSDB_LOG_READ || file->write_error) {
        file->mode = OVSDB_LOG_WRITE;
        file->write_error = false;
        if (fseeko(file->stream, file->offset, SEEK_SET)) {
            error = ovsdb_io_error(errno, "%s: cannot seek to offset %lld",
                                   file->name, (long long int) file->offset);
            goto error;
        }
        if (ftruncate(fileno(file->stream), file->offset)) {
            error = ovsdb_io_error(errno, "%s: cannot truncate to length %lld",
                                   file->name, (long long int) file->offset);
            goto error;
        }
    }

    if (json->type != JSON_OBJECT && json->type != JSON_ARRAY) {
        error = OVSDB_BUG("bad JSON type");
        goto error;
    }

    struct ds header = DS_EMPTY_INITIALIZER;
    struct ds data = DS_EMPTY_INITIALIZER;
    ovsdb_log_compose_record(json, file->magic, &header, &data);
    size_t total_length = header.length + data.length;

    /* Write. */
    bool ok = (fwrite(header.string, header.length, 1, file->stream) == 1
               && fwrite(data.string, data.length, 1, file->stream) == 1
               && fflush(file->stream) == 0);
    ds_destroy(&header);
    ds_destroy(&data);
    if (!ok) {
        error = ovsdb_io_error(errno, "%s: write failed", file->name);

        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);
        VLOG_WARN_RL(&rl, "%s: write failed (%s)",
                     file->name, ovs_strerror(errno));

        /* Remove any partially written data, ignoring errors since there is
         * nothing further we can do. */
        ignore(ftruncate(fileno(file->stream), file->offset));

        goto error;
    }

    file->offset += total_length;
    return NULL;

error:
    file->write_error = true;
    return error;
}

struct ovsdb_error *
ovsdb_log_commit(struct ovsdb_log *file)
{
    if (fsync(fileno(file->stream))) {
        return ovsdb_io_error(errno, "%s: fsync failed", file->name);
    }
    return NULL;
}

/* Returns the current offset into the file backing 'log', in bytes.  This
 * reflects the number of bytes that have been read or written in the file.  If
 * the whole file has been read, this is the file size. */
off_t
ovsdb_log_get_offset(const struct ovsdb_log *log)
{
    return log->offset;
}

struct ovsdb_error * OVS_WARN_UNUSED_RESULT
ovsdb_log_replace(struct ovsdb_log *log, struct json **entries, size_t n)
{
    struct ovsdb_error *error;
    struct ovsdb_log *new;

    error = ovsdb_log_replace_start(log, &new);
    if (error) {
        return error;
    }

    for (size_t i = 0; i < n; i++) {
        error = ovsdb_log_write(new, entries[i]);
        if (error) {
            ovsdb_log_replace_abort(new);
            return error;
        }
    }

    return ovsdb_log_replace_commit(log, new);
}

struct ovsdb_error * OVS_WARN_UNUSED_RESULT
ovsdb_log_replace_start(struct ovsdb_log *old,
                        struct ovsdb_log **newp)
{
    char *tmp_name = xasprintf("%s.tmp", old->name);
    struct ovsdb_error *error;

    ovs_assert(old->lockfile);

    /* Remove temporary file.  (It might not exist.) */
    if (unlink(tmp_name) < 0 && errno != ENOENT) {
        error = ovsdb_io_error(errno, "failed to remove %s", tmp_name);
        free(tmp_name);
        *newp = NULL;
        return error;
    }

    /* Create temporary file. */
    error = ovsdb_log_open(tmp_name, old->magic, OVSDB_LOG_CREATE_EXCL,
                           false, newp);
    free(tmp_name);
    return error;
}

struct ovsdb_error * OVS_WARN_UNUSED_RESULT
ovsdb_log_replace_commit(struct ovsdb_log *old, struct ovsdb_log *new)
{
    struct ovsdb_error *error = ovsdb_log_commit(new);
    if (error) {
        ovsdb_log_close(new);
        return error;
    }

    /* Replace old file by new file on-disk. */
    if (rename(new->name, old->name)) {
        error = ovsdb_io_error(errno, "failed to rename \"%s\" to \"%s\"",
                               new->name, old->name);
        ovsdb_log_close(new);
        return error;
    }
    fsync_parent_dir(old->name);

    /* Replace 'old' by 'new' in memory.
     *
     * 'old' transitions to OVSDB_LOG_WRITE (it was probably in that mode
     * anyway). */
    /* prev_offset only matters for OVSDB_LOG_READ. */
    old->offset = new->offset;
    /* Keep old->name and old->rel_name. */
    free(old->magic);
    old->magic = new->magic;
    new->magic = NULL;
    /* Keep old->lockfile. */
    fclose(old->stream);
    old->stream = new->stream;
    new->stream = NULL;
    /* read_error only matters for OVSDB_LOG_READ. */
    old->write_error = new->write_error;
    old->mode = OVSDB_LOG_WRITE;

    /* Free 'new'. */
    ovsdb_log_close(new);

    return NULL;
}

void
ovsdb_log_replace_abort(struct ovsdb_log *new)
{
    if (new) {
        /* Unlink the new file, but only after we close it (for Windows
         * compatibility). */
        char *name = xstrdup(new->name);
        ovsdb_log_close(new);
        unlink(name);
        free(name);
    }
}
