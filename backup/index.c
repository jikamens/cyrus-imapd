/* index.c -- replication-based backup api - index functions
 *
 * Copyright (c) 1994-2015 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include <assert.h>

#include "lib/xmalloc.h"

#include "imap/dlist.h"
#include "imap/imap_err.h"

#include "backup/api.h"
#include "backup/sqlconsts.h"

#define BACKUP_INTERNAL_SOURCE /* this file is part of the backup API */
#include "backup/internal.h"

static int _index_mailbox(struct backup *backup, struct dlist *dl,
                          time_t ts, off_t dl_offset);
static int _index_message(sqldb_t *db, int backup_id, struct dlist *dl,
                          off_t dl_offset, size_t dl_len);

HIDDEN int backup_index(struct backup *backup, struct dlist *dlist,
                        time_t ts, off_t start, size_t len)
{
    int r;

    if (0) { }

    else if (strcmp(dlist->name, "MAILBOX") == 0)
        r = _index_mailbox(backup, dlist, ts, start);
    else if (strcmp(dlist->name, "MESSAGE") == 0)
        r = _index_message(backup->db, backup->append_state->index_id, dlist, start, len);

    else {
        fprintf(stderr, "ignoring unrecognised dlist name: %s\n", dlist->name);
        r = -1; // FIXME
    }

    return r;
}

static int _get_magic_flags(struct dlist *flags, int *is_expunged)
{
    struct dlist *found_expunged = NULL;
    struct dlist *di;
    int found = 0;

    assert(strcmp(flags->name, "FLAGS") == 0);

    for (di = flags->head; di; di = di->next) {
        if (strcmp(di->sval, "\\Expunged") == 0) {
            if (is_expunged) *is_expunged = 1;
            found_expunged = di;
            found++;
        }
    }

    if (found_expunged) {
        dlist_unstitch(flags, found_expunged);
        dlist_free(&found_expunged);
    }

    return found;
}

static int _index_mailbox(struct backup *backup, struct dlist *dl,
                          time_t ts, off_t dl_offset)
{
    fprintf(stderr, "indexing MAILBOX at " OFF_T_FMT "...\n", dl_offset);

    const char *uniqueid = NULL;
    const char *mboxname = NULL;
    const char *mboxtype = NULL;
    uint32_t last_uid = 0;
    modseq_t highestmodseq = 0;
    uint32_t recentuid = 0;
    time_t recenttime = 0;
    time_t last_appenddate = 0;
    time_t pop3_last_login = 0;
    time_t pop3_show_after = 0;
    uint32_t uidvalidity = 0;
    const char *partition = NULL;
    const char *acl = NULL;
    const char *options = NULL;
    struct synccrcs synccrcs = { 0, 0 };
    const char *quotaroot = NULL;
    modseq_t xconvmodseq = 0;
    struct dlist *annotations = NULL;
    struct buf annotations_buf = BUF_INITIALIZER;
    struct dlist *record = NULL;

    if (!dlist_getatom(dl, "UNIQUEID", &uniqueid))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getatom(dl, "MBOXNAME", &mboxname))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getnum32(dl, "LAST_UID", &last_uid))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getnum64(dl, "HIGHESTMODSEQ", &highestmodseq))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getnum32(dl, "RECENTUID", &recentuid))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getdate(dl, "RECENTTIME", &recenttime))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getdate(dl, "LAST_APPENDDATE", &last_appenddate))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getdate(dl, "POP3_LAST_LOGIN", &pop3_last_login))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getnum32(dl, "UIDVALIDITY", &uidvalidity))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getatom(dl, "PARTITION", &partition))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getatom(dl, "ACL", &acl))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getatom(dl, "OPTIONS", &options))
        return IMAP_PROTOCOL_BAD_PARAMETERS;
    if (!dlist_getlist(dl, "RECORD", &record))
        return IMAP_PROTOCOL_BAD_PARAMETERS;

    /* optional */
    dlist_getlist(dl, "ANNOTATIONS", &annotations);
    dlist_getdate(dl, "POP3_SHOW_AFTER", &pop3_show_after);
    dlist_getatom(dl, "MBOXTYPE", &mboxtype);
    dlist_getnum64(dl, "XCONVMODSEQ", &xconvmodseq);

    /* CRCs */
    dlist_getnum32(dl, "SYNC_CRC", &synccrcs.basic);
    dlist_getnum32(dl, "SYNC_CRC_ANNOT", &synccrcs.annot);

    if (annotations) {
        dlist_printbuf(annotations, 0, &annotations_buf);
    }

    struct sqldb_bindval mbox_bval[] = {
        { ":last_backup_id",    SQLITE_INTEGER, { .i = backup->append_state->index_id } },
        { ":uniqueid",          SQLITE_TEXT,    { .s = uniqueid } },
        { ":mboxname",          SQLITE_TEXT,    { .s = mboxname } },
        { ":mboxtype",          SQLITE_TEXT,    { .s = mboxtype } },
        { ":last_uid",          SQLITE_INTEGER, { .i = last_uid } },
        { ":highestmodseq",     SQLITE_INTEGER, { .i = highestmodseq } },
        { ":recentuid",         SQLITE_INTEGER, { .i = recentuid } },
        { ":recenttime",        SQLITE_INTEGER, { .i = recenttime } },
        { ":last_appenddate",   SQLITE_INTEGER, { .i = last_appenddate } },
        { ":pop3_last_login",   SQLITE_INTEGER, { .i = pop3_last_login } },
        { ":pop3_show_after",   SQLITE_INTEGER, { .i = pop3_show_after } },
        { ":uidvalidity",       SQLITE_INTEGER, { .i = uidvalidity } },
        { ":partition",         SQLITE_TEXT,    { .s = partition } },
        { ":acl",               SQLITE_TEXT,    { .s = acl } },
        { ":options",           SQLITE_TEXT,    { .s = options } },
        { ":sync_crc",          SQLITE_INTEGER, { .i = synccrcs.basic } },
        { ":sync_crc_annot",    SQLITE_INTEGER, { .i = synccrcs.annot } },
        { ":quotaroot",         SQLITE_TEXT,    { .s = quotaroot } },
        { ":xconvmodseq",       SQLITE_INTEGER, { .i = xconvmodseq } },
        { ":annotations",       SQLITE_TEXT,    { .s = buf_cstring(&annotations_buf) } },
        { ":deleted",           SQLITE_INTEGER, { .i = 0 } },
        { NULL,                 SQLITE_NULL,    { .s = NULL      } },
    };

    buf_free(&annotations_buf);

    sqldb_begin(backup->db, __func__); // FIXME what if this fails

    int r = sqldb_exec(backup->db, backup_index_mailbox_update_sql,
                       mbox_bval, NULL, NULL);
    if (r) {
        // FIXME handle this sensibly
        fprintf(stderr, "%s: something went wrong: %i update %s\n", __func__, r, mboxname);
        goto error;
    }
    if (sqldb_changes(backup->db) == 0) {
        r = sqldb_exec(backup->db, backup_index_mailbox_insert_sql,
                       mbox_bval, NULL, NULL);
        if (r) {
            // FIXME handle this sensibly
            fprintf(stderr, "%s: something went wrong: %i insert %s\n", __func__, r, mboxname);
            goto error;
        }
    }

    int mailbox_id = backup_get_mailbox_id(backup, uniqueid);

    struct dlist *ki = NULL;

    for (ki = record->head; ki; ki = ki->next) {
        uint32_t uid = 0;
        modseq_t modseq = 0;
        uint32_t last_updated = 0;
        struct dlist *flags = NULL;
        struct buf flags_buf = BUF_INITIALIZER;
        uint32_t internaldate;
        uint32_t size;
        const char *guid;
        struct dlist *annotations = NULL;
        struct buf annotations_buf = BUF_INITIALIZER;
        int message_id = -1;
        time_t expunged = 0;

        if (!dlist_getnum32(ki, "UID", &uid))
            goto error;
        if (!dlist_getnum64(ki, "MODSEQ", &modseq))
            goto error;
        if (!dlist_getnum32(ki, "LAST_UPDATED", &last_updated))
            goto error;
        if (!dlist_getnum32(ki, "INTERNALDATE", &internaldate))
            goto error;
        if (!dlist_getnum32(ki, "SIZE", &size))
            goto error;
        if (!dlist_getatom(ki, "GUID", &guid))
            goto error;

        dlist_getlist(ki, "FLAGS", &flags);
        if (flags) {
            int is_expunged = 0;

            _get_magic_flags(flags, &is_expunged);

            if (is_expunged) {
                fprintf(stderr, "%s: found expunge flag for message %s\n", __func__, guid);
                expunged = ts;
            }

            dlist_printbuf(flags, 0, &flags_buf);
            fprintf(stderr, "%s: found flags for message %s: %s\n", __func__, guid, buf_cstring(&flags_buf));
        }

        dlist_getlist(ki, "ANNOTATIONS", &annotations);
        if (annotations) {
            dlist_printbuf(annotations, 0, &annotations_buf);
        }

        message_id = backup_get_message_id(backup, guid);
        if (message_id == -1) {
            // FIXME handle this sensibly
            fprintf(stderr, "%s: something went wrong: %i %s %s\n", __func__, r, mboxname, guid);
            goto error;
        }

        struct sqldb_bindval record_bval[] = {
            { ":mailbox_id",        SQLITE_INTEGER, { .i = mailbox_id } },
            { ":message_id",        SQLITE_INTEGER, { .i = message_id } },
            { ":last_backup_id",    SQLITE_INTEGER, { .i = backup->append_state->index_id } },
            { ":uid",               SQLITE_INTEGER, { .i = uid } },
            { ":modseq",            SQLITE_INTEGER, { .i = modseq } },
            { ":last_updated",      SQLITE_INTEGER, { .i = last_updated } },
            { ":flags",             SQLITE_TEXT,    { .s = buf_cstring(&flags_buf) } },
            { ":internaldate",      SQLITE_INTEGER, { .i = internaldate } },
            { ":annotations",       SQLITE_TEXT,    { .s = buf_cstring(&annotations_buf) } },
            { ":expunged",          SQLITE_NULL,    { .s = NULL      } },
            { NULL,                 SQLITE_NULL,    { .s = NULL      } },
        };

        /* provide an expunged value if we have one */
        if (expunged) {
            struct sqldb_bindval *expunged_bval = &record_bval[9];
            assert(strcmp(expunged_bval->name, ":expunged") == 0);
            expunged_bval->type = SQLITE_INTEGER;
            expunged_bval->val.i = expunged;
        }

        r = sqldb_exec(backup->db, backup_index_mailbox_message_update_sql,
                       record_bval, NULL, NULL);

        buf_free(&annotations_buf);
        buf_free(&flags_buf);

        if (r) {
            // FIXME handle this sensibly
            fprintf(stderr, "%s: something went wrong: %i update %s %s\n", __func__, r, mboxname, guid);
            goto error;
        }
        if (sqldb_changes(backup->db) == 0) {
            r = sqldb_exec(backup->db, backup_index_mailbox_message_insert_sql,
                           record_bval, NULL, NULL);
            if (r) {
                // FIXME handle this sensibly
                fprintf(stderr, "%s: something went wrong: %i insert %s %s\n", __func__, r, mboxname, guid);
                goto error;
            }
        }
    }

    fprintf(stderr, "%s: committing index change: %s\n", __func__, mboxname);
    sqldb_commit(backup->db, __func__);
    return 0;

error:
    fprintf(stderr, "%s: rolling back index change: %s\n", __func__, mboxname);
    sqldb_rollback(backup->db, __func__);

    return -1;
}

static int _index_message(sqldb_t *db, int backup_id, struct dlist *dl,
                          off_t dl_offset, size_t dl_len)
{
    fprintf(stderr, "indexing MESSAGE at " OFF_T_FMT " (" SIZE_T_FMT " bytes)...\n", dl_offset, dl_len);

    struct dlist *ki;

    /* n.b. APPLY MESSAGE contains a list of messages, not just one */
    for (ki = dl->head; ki; ki = ki->next) {
        if (ki->type != DL_SFILE)
            continue;

        // FIXME DL_SFILEs have the offset and size already recorded
        // so we could use that...
        // but, it's the offset in the input stream which is fine
        // for reindex (input stream = gz data), but useless for
        // backupd (input stream = remote sync_client).

        char *guid = xstrdup(message_guid_encode(ki->gval));
        char *partition = ki->part;

        struct sqldb_bindval bval[] = {
            { ":guid",      SQLITE_TEXT,    { .s = guid      } },
            { ":partition", SQLITE_TEXT,    { .s = partition } },
            { ":backup_id", SQLITE_INTEGER, { .i = backup_id } },
            { ":offset",    SQLITE_INTEGER, { .i = dl_offset } },
            { ":length",    SQLITE_INTEGER, { .i = dl_len    } },
            { NULL,         SQLITE_NULL,    { .s = NULL      } },
        };

        int r = sqldb_exec(db, backup_index_message_insert_sql, bval, NULL,
                           NULL);
        if (r) {
            // FIXME handle this sensibly
            fprintf(stderr, "%s: something went wrong: %i %s\n", __func__, r, guid);
        }

        free(guid);
    }

    return 0;
}