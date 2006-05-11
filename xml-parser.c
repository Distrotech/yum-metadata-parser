/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <string.h>
#include <glib.h>
#include <sqlite3.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "xml-parser.h"
#include "debug.h"

#define PACKAGE_FIELD_SIZE 1024

typedef enum {
    PRIMARY_PARSER_TOPLEVEL = 0,
    PRIMARY_PARSER_PACKAGE,
    PRIMARY_PARSER_FORMAT,
    PRIMARY_PARSER_DEP,
} PrimarySAXContextState;

typedef struct {
    xmlParserCtxt *xml_context;
    PrimarySAXContextState state;
    PackageFn callback;
    gpointer user_data;

    Package *current_package;
    GSList **current_dep_list;
    PackageFile *current_file;

    GString *text_buffer;
} PrimarySAXContext;

static void
primary_parser_toplevel_start (PrimarySAXContext *ctx,
                               const char *name,
                               const char **attrs)
{
    if (!strcmp (name, "package")) {
        g_assert (ctx->current_package == NULL);

        ctx->state = PRIMARY_PARSER_PACKAGE;

        ctx->current_package = package_new ();
    }
}

static void
primary_parser_package_start (PrimarySAXContext *ctx,
                              const char *name,
                              const char **attrs)
{
    Package *p = ctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    if (!strcmp (name, "format")) {
        ctx->state = PRIMARY_PARSER_FORMAT;
    }

    else if (!strcmp (name, "version")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "epoch"))
                p->epoch = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "ver"))
                p->version = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "rel"))
                p->release = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "checksum")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "type"))
                p->checksum_type = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "time")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "file"))
                p->time_file = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "build"))
                p->time_build = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "size")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "package"))
                p->size_package = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "installed"))
                p->size_installed = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "archive"))
                p->size_archive = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "location")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "href"))
                p->location_href = g_string_chunk_insert (p->chunk, value);;
        }
    }
}

static void
primary_parser_format_start (PrimarySAXContext *ctx,
                             const char *name,
                             const char **attrs)
{
    Package *p = ctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    if (!strcmp (name, "rpm:header-range")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "start"))
                p->rpm_header_start = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "end"))
                p->rpm_header_end = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "rpm:provides")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &ctx->current_package->provides;
    } else if (!strcmp (name, "rpm:requires")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &ctx->current_package->requires;
    } else if (!strcmp (name, "rpm:obsoletes")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &ctx->current_package->obsoletes;
    } else if (!strcmp (name, "rpm:conflicts")) {
        ctx->state = PRIMARY_PARSER_DEP;
        ctx->current_dep_list = &ctx->current_package->conflicts;
    }

    else if (!strcmp (name, "file")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "type")) {
                ctx->current_file = package_file_new ();
                ctx->current_file->type =
                    g_string_chunk_insert_const (p->chunk, value);
            }
        }
    }
}

static void
primary_parser_dep_start (PrimarySAXContext *ctx,
                          const char *name,
                          const char **attrs)
{
    const char *tmp_name = NULL;
    const char *tmp_version = NULL;
    const char *tmp_release = NULL;
    const char *tmp_epoch = NULL;
    const char *tmp_flags = NULL;
    Dependency *dep;
    int i;
    gboolean ignore = FALSE;
    const char *attr;
    const char *value;

    if (!strcmp (name, "rpm:entry")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "name")) {
                if (!strncmp (value, "rpmlib(", strlen ("rpmlib("))) {
                    ignore = TRUE;
                    break;
                }
                tmp_name = value;
            } else if (!strcmp (attr, "flags"))
                tmp_flags = value;
            else if (!strcmp (attr, "epoch"))
                tmp_epoch = value;
            else if (!strcmp (attr, "ver"))
                tmp_version = value;
            else if (!strcmp (attr, "rel"))
                tmp_release = value;
        }

        if (!ignore) {
            GStringChunk *chunk = ctx->current_package->chunk;

            dep = dependency_new ();
            dep->name = g_string_chunk_insert (chunk, tmp_name);
            if (tmp_flags)
                dep->flags = g_string_chunk_insert (chunk, tmp_flags);
            if (tmp_epoch)
                dep->epoch = g_string_chunk_insert (chunk, tmp_epoch);
            if (tmp_version)
                dep->version = g_string_chunk_insert (chunk, tmp_version);
            if (tmp_release)
                dep->release = g_string_chunk_insert (chunk, tmp_release);

            *ctx->current_dep_list = g_slist_prepend (*ctx->current_dep_list,
                                                      dep);
        }
    }
}

static void
primary_sax_start_element (void *data, const char *name, const char **attrs)
{
    PrimarySAXContext *ctx = (PrimarySAXContext *) data;

    if (ctx->text_buffer->len)
        g_string_truncate (ctx->text_buffer, 0);

    switch (ctx->state) {
    case PRIMARY_PARSER_TOPLEVEL:
        primary_parser_toplevel_start (ctx, name, attrs);
        break;
    case PRIMARY_PARSER_PACKAGE:
        primary_parser_package_start (ctx, name, attrs);
        break;
    case PRIMARY_PARSER_FORMAT:
        primary_parser_format_start (ctx, name, attrs);
        break;
    case PRIMARY_PARSER_DEP:
        primary_parser_dep_start (ctx, name, attrs);
        break;

    default:
        break;
    }
}

static void
primary_parser_package_end (PrimarySAXContext *ctx, const char *name)
{
    Package *p = ctx->current_package;

    g_assert (p != NULL);

    if (!strcmp (name, "package")) {
        if (ctx->callback)
            ctx->callback (p, ctx->user_data);

        package_free (p);
        ctx->current_package = NULL;

        ctx->state = PRIMARY_PARSER_TOPLEVEL;
    }

    else if (ctx->text_buffer->len == 0)
        /* Nothing interesting to do here */
        return;

    else if (!strcmp (name, "name"))
        p->name = g_string_chunk_insert_len (p->chunk,
                                             ctx->text_buffer->str,
                                             ctx->text_buffer->len);
    else if (!strcmp (name, "arch"))
        p->arch = g_string_chunk_insert_len (p->chunk,
                                             ctx->text_buffer->str,
                                             ctx->text_buffer->len);
    else if (!strcmp (name, "checksum"))
        p->checksum_value = p->pkgId =
            g_string_chunk_insert_len (p->chunk,
                                       ctx->text_buffer->str,
                                       ctx->text_buffer->len);
    else if (!strcmp (name, "summary"))
        p->summary = g_string_chunk_insert_len (p->chunk,
                                                ctx->text_buffer->str,
                                                ctx->text_buffer->len);
    else if (!strcmp (name, "description"))
        p->description = g_string_chunk_insert_len (p->chunk,
                                                    ctx->text_buffer->str,
                                                    ctx->text_buffer->len);
    else if (!strcmp (name, "packager"))
        p->rpm_packager = g_string_chunk_insert_len (p->chunk,
                                                     ctx->text_buffer->str,
                                                     ctx->text_buffer->len);
    else if (!strcmp (name, "url"))
        p->url = g_string_chunk_insert_len (p->chunk,
                                            ctx->text_buffer->str,
                                            ctx->text_buffer->len);
}

static void
primary_parser_format_end (PrimarySAXContext *ctx, const char *name)
{
    Package *p = ctx->current_package;

    g_assert (p != NULL);

    if (!strcmp (name, "rpm:license"))
        p->rpm_license = g_string_chunk_insert_len (p->chunk,
                                                    ctx->text_buffer->str,
                                                    ctx->text_buffer->len);
    if (!strcmp (name, "rpm:vendor"))
        p->rpm_vendor = g_string_chunk_insert_len (p->chunk,
                                                   ctx->text_buffer->str,
                                                   ctx->text_buffer->len);
    if (!strcmp (name, "rpm:group"))
        p->rpm_group = g_string_chunk_insert_len (p->chunk,
                                                  ctx->text_buffer->str,
                                                  ctx->text_buffer->len);
    if (!strcmp (name, "rpm:buildhost"))
        p->rpm_buildhost = g_string_chunk_insert_len (p->chunk,
                                                      ctx->text_buffer->str,
                                                      ctx->text_buffer->len);
    if (!strcmp (name, "rpm:sourcerpm"))
        p->rpm_sourcerpm = g_string_chunk_insert_len (p->chunk,
                                                      ctx->text_buffer->str,
                                                      ctx->text_buffer->len);
    else if (!strcmp (name, "file")) {
        PackageFile *file = ctx->current_file != NULL ?
            ctx->current_file : package_file_new ();

        file->name = g_string_chunk_insert_len (p->chunk,
                                                ctx->text_buffer->str,
                                                ctx->text_buffer->len);

        if (!file->type)
            file->type = g_string_chunk_insert_const (p->chunk, "file");

        p->files = g_slist_prepend (p->files, file);
        ctx->current_file = NULL;
    } else if (!strcmp (name, "format"))
        ctx->state = PRIMARY_PARSER_PACKAGE;
}

static void
primary_parser_dep_end (PrimarySAXContext *ctx, const char *name)
{
    g_assert (ctx->current_package != NULL);

    if (strcmp (name, "rpm:entry"))
        ctx->state = PRIMARY_PARSER_FORMAT;
}

static void
primary_sax_end_element (void *data, const char *name)
{
    PrimarySAXContext *ctx = (PrimarySAXContext *) data;

    switch (ctx->state) {
    case PRIMARY_PARSER_PACKAGE:
        primary_parser_package_end (ctx, name);
        break;
    case PRIMARY_PARSER_FORMAT:
        primary_parser_format_end (ctx, name);
        break;
    case PRIMARY_PARSER_DEP:
        primary_parser_dep_end (ctx, name);
        break;
    default:
        break;
    }

    g_string_truncate (ctx->text_buffer, 0);
}

static void
primary_sax_characters (void *data, const char *ch, int len)
{
    PrimarySAXContext *ctx = (PrimarySAXContext *) data;

    g_string_append_len (ctx->text_buffer, ch, len);
}

static void
primary_sax_warning (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    debug (DEBUG_LEVEL_WARNING, "* SAX Warning: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static void
primary_sax_error (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    debug (DEBUG_LEVEL_ERROR, "* SAX Error: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static xmlSAXHandler primary_sax_handler = {
    NULL,      /* internalSubset */
    NULL,      /* isStandalone */
    NULL,      /* hasInternalSubset */
    NULL,      /* hasExternalSubset */
    NULL,      /* resolveEntity */
    NULL,      /* getEntity */
    NULL,      /* entityDecl */
    NULL,      /* notationDecl */
    NULL,      /* attributeDecl */
    NULL,      /* elementDecl */
    NULL,      /* unparsedEntityDecl */
    NULL,      /* setDocumentLocator */
    NULL,      /* startDocument */
    NULL,      /* endDocument */
    (startElementSAXFunc) primary_sax_start_element, /* startElement */
    (endElementSAXFunc) primary_sax_end_element,     /* endElement */
    NULL,      /* reference */
    (charactersSAXFunc) primary_sax_characters,      /* characters */
    NULL,      /* ignorableWhitespace */
    NULL,      /* processingInstruction */
    NULL,      /* comment */
    primary_sax_warning,      /* warning */
    primary_sax_error,      /* error */
    primary_sax_error,      /* fatalError */
};

void
yum_xml_parse_primary (const char *filename,
                       PackageFn callback,
                       gpointer user_data)
{
    PrimarySAXContext ctx;
    int rc;

    ctx.state = PRIMARY_PARSER_TOPLEVEL;
    ctx.callback = callback;
    ctx.user_data = user_data;
    ctx.current_package = NULL;
    ctx.current_dep_list = NULL;
    ctx.current_file = NULL;
    ctx.text_buffer = g_string_sized_new (PACKAGE_FIELD_SIZE);

    xmlSubstituteEntitiesDefault (1);
    rc = xmlSAXUserParseFile (&primary_sax_handler, &ctx, filename);

    if (ctx.current_package) {
        debug (DEBUG_LEVEL_WARNING, "Incomplete package lost");
        package_free (ctx.current_package);
    }

    g_string_free (ctx.text_buffer, TRUE);
}

/*****************************************************************************/


typedef enum {
    FILELIST_PARSER_TOPLEVEL = 0,
    FILELIST_PARSER_PACKAGE,
} FilelistSAXContextState;

typedef struct {
    xmlParserCtxt *xml_context;
    FilelistSAXContextState state;
    PackageFn callback;
    gpointer user_data;

    Package *current_package;
    PackageFile *current_file;

    GString *text_buffer;
} FilelistSAXContext;

static void
filelist_parser_toplevel_start (FilelistSAXContext *ctx,
                                const char *name,
                                const char **attrs)
{
    if (!strcmp (name, "package")) {
        Package *p;
        int i;
        const char *attr;
        const char *value;

        g_assert (ctx->current_package == NULL);

        ctx->state = FILELIST_PARSER_PACKAGE;

        ctx->current_package = p = package_new ();

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "pkgid"))
                p->pkgId = g_string_chunk_insert (p->chunk, value);
            if (!strcmp (attr, "name"))
                p->name = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "arch"))
                p->arch = g_string_chunk_insert (p->chunk, value);
        }
    }
}

static void
filelist_parser_package_start (FilelistSAXContext *ctx,
                               const char *name,
                               const char **attrs)
{
    Package *p = ctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    if (!strcmp (name, "version")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "epoch"))
                p->epoch = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "ver"))
                p->version = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "rel"))
                p->release = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "file")) {
        ctx->current_file = package_file_new ();

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "type"))
                ctx->current_file->type =
                    g_string_chunk_insert_const (p->chunk, value);
        }
    }
}

static void
filelist_sax_start_element (void *data, const char *name, const char **attrs)
{
    FilelistSAXContext *ctx = (FilelistSAXContext *) data;

    if (ctx->text_buffer->len)
        g_string_truncate (ctx->text_buffer, 0);

    switch (ctx->state) {
    case FILELIST_PARSER_TOPLEVEL:
        filelist_parser_toplevel_start (ctx, name, attrs);
        break;
    case FILELIST_PARSER_PACKAGE:
        filelist_parser_package_start (ctx, name, attrs);
        break;
    default:
        break;
    }
}

static void
filelist_parser_package_end (FilelistSAXContext *ctx, const char *name)
{
    Package *p = ctx->current_package;

    g_assert (p != NULL);

    if (!strcmp (name, "package")) {
        if (ctx->callback)
            ctx->callback (p, ctx->user_data);

        package_free (p);
        ctx->current_package = NULL;

        if (ctx->current_file) {
            g_free (ctx->current_file);
            ctx->current_file = NULL;
        }

        ctx->state = FILELIST_PARSER_TOPLEVEL;
    }

    else if (!strcmp (name, "file")) {
        PackageFile *file = ctx->current_file;
        file->name = g_string_chunk_insert_len (p->chunk,
                                                ctx->text_buffer->str,
                                                ctx->text_buffer->len);
        if (!file->type)
            file->type = g_string_chunk_insert_const (p->chunk, "file");

        p->files = g_slist_prepend (p->files, file);
        ctx->current_file = NULL;
    }
}

static void
filelist_sax_end_element (void *data, const char *name)
{
    FilelistSAXContext *ctx = (FilelistSAXContext *) data;

    switch (ctx->state) {
    case FILELIST_PARSER_PACKAGE:
        filelist_parser_package_end (ctx, name);
        break;
    default:
        break;
    }

    g_string_truncate (ctx->text_buffer, 0);
}

static void
filelist_sax_characters (void *data, const char *ch, int len)
{
    FilelistSAXContext *ctx = (FilelistSAXContext *) data;

    g_string_append_len (ctx->text_buffer, ch, len);
}

static void
filelist_sax_warning (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    debug (DEBUG_LEVEL_WARNING, "* SAX Warning: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static void
filelist_sax_error (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    debug (DEBUG_LEVEL_ERROR, "* SAX Error: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static xmlSAXHandler filelist_sax_handler = {
    NULL,      /* internalSubset */
    NULL,      /* isStandalone */
    NULL,      /* hasInternalSubset */
    NULL,      /* hasExternalSubset */
    NULL,      /* resolveEntity */
    NULL,      /* getEntity */
    NULL,      /* entityDecl */
    NULL,      /* notationDecl */
    NULL,      /* attributeDecl */
    NULL,      /* elementDecl */
    NULL,      /* unparsedEntityDecl */
    NULL,      /* setDocumentLocator */
    NULL,      /* startDocument */
    NULL,      /* endDocument */
    (startElementSAXFunc) filelist_sax_start_element, /* startElement */
    (endElementSAXFunc) filelist_sax_end_element,     /* endElement */
    NULL,      /* reference */
    (charactersSAXFunc) filelist_sax_characters,      /* characters */
    NULL,      /* ignorableWhitespace */
    NULL,      /* processingInstruction */
    NULL,      /* comment */
    filelist_sax_warning,      /* warning */
    filelist_sax_error,      /* error */
    filelist_sax_error,      /* fatalError */
};

void
yum_xml_parse_filelists (const char *filename,
                         PackageFn callback,
                         gpointer user_data)
{
    FilelistSAXContext ctx;
    int rc;

    ctx.state = FILELIST_PARSER_TOPLEVEL;
    ctx.callback = callback;
    ctx.user_data = user_data;
    ctx.current_package = NULL;
    ctx.current_file = NULL;
    ctx.text_buffer = g_string_sized_new (PACKAGE_FIELD_SIZE);

    xmlSubstituteEntitiesDefault (1);
    rc = xmlSAXUserParseFile (&filelist_sax_handler, &ctx, filename);

    if (ctx.current_package) {
        debug (DEBUG_LEVEL_WARNING, "Incomplete package lost");
        package_free (ctx.current_package);
    }

    if (ctx.current_file)
        g_free (ctx.current_file);

    g_string_free (ctx.text_buffer, TRUE);
}

/*****************************************************************************/

typedef enum {
    OTHER_PARSER_TOPLEVEL = 0,
    OTHER_PARSER_PACKAGE,
} OtherSAXContextState;

typedef struct {
    xmlParserCtxt *xml_context;
    OtherSAXContextState state;
    PackageFn callback;
    gpointer user_data;

    Package *current_package;
    ChangelogEntry *current_entry;

    GString *text_buffer;
} OtherSAXContext;

static void
other_parser_toplevel_start (OtherSAXContext *ctx,
                             const char *name,
                             const char **attrs)
{
    if (!strcmp (name, "package")) {
        Package *p;
        int i;
        const char *attr;
        const char *value;

        g_assert (ctx->current_package == NULL);

        ctx->state = OTHER_PARSER_PACKAGE;

        ctx->current_package = p = package_new ();

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "pkgid"))
                p->pkgId = g_string_chunk_insert (p->chunk, value);
            if (!strcmp (attr, "name"))
                p->name = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "arch"))
                p->arch = g_string_chunk_insert (p->chunk, value);
        }
    }
}

static void
other_parser_package_start (OtherSAXContext *ctx,
                            const char *name,
                            const char **attrs)
{
    Package *p = ctx->current_package;
    int i;
    const char *attr;
    const char *value;

    g_assert (p != NULL);

    if (!strcmp (name, "version")) {
        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "epoch"))
                p->epoch = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "ver"))
                p->version = g_string_chunk_insert (p->chunk, value);
            else if (!strcmp (attr, "rel"))
                p->release = g_string_chunk_insert (p->chunk, value);
        }
    }

    else if (!strcmp (name, "changelog")) {
        ctx->current_entry = changelog_entry_new ();

        for (i = 0; attrs && attrs[i]; i++) {
            attr = attrs[i];
            value = attrs[++i];

            if (!strcmp (attr, "author"))
                ctx->current_entry->author =
                    g_string_chunk_insert_const (p->chunk, value);
            else if (!strcmp (attr, "date"))
                ctx->current_entry->date =
                    g_string_chunk_insert_const (p->chunk, value);
        }
    }
}

static void
other_sax_start_element (void *data, const char *name, const char **attrs)
{
    OtherSAXContext *ctx = (OtherSAXContext *) data;

    if (ctx->text_buffer->len)
        g_string_truncate (ctx->text_buffer, 0);

    switch (ctx->state) {
    case OTHER_PARSER_TOPLEVEL:
        other_parser_toplevel_start (ctx, name, attrs);
        break;
    case OTHER_PARSER_PACKAGE:
        other_parser_package_start (ctx, name, attrs);
        break;
    default:
        break;
    }
}

static void
other_parser_package_end (OtherSAXContext *ctx, const char *name)
{
    Package *p = ctx->current_package;

    g_assert (p != NULL);

    if (!strcmp (name, "package")) {

        if (p->changelogs)
            p->changelogs = g_slist_reverse (p->changelogs);

        if (ctx->callback)
            ctx->callback (p, ctx->user_data);

        package_free (p);
        ctx->current_package = NULL;

        if (ctx->current_entry) {
            g_free (ctx->current_entry);
            ctx->current_entry = NULL;
        }

        ctx->state = OTHER_PARSER_TOPLEVEL;
    }

    else if (!strcmp (name, "changelog")) {
        ctx->current_entry->changelog =
            g_string_chunk_insert_len (p->chunk,
                                       ctx->text_buffer->str,
                                       ctx->text_buffer->len);

        p->changelogs = g_slist_prepend (p->changelogs, ctx->current_entry);
        ctx->current_entry = NULL;
    }
}

static void
other_sax_end_element (void *data, const char *name)
{
    OtherSAXContext *ctx = (OtherSAXContext *) data;

    switch (ctx->state) {
    case OTHER_PARSER_PACKAGE:
        other_parser_package_end (ctx, name);
        break;
    default:
        break;
    }

    g_string_truncate (ctx->text_buffer, 0);
}

static void
other_sax_characters (void *data, const char *ch, int len)
{
    OtherSAXContext *ctx = (OtherSAXContext *) data;

    g_string_append_len (ctx->text_buffer, ch, len);
}

static void
other_sax_warning (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    debug (DEBUG_LEVEL_WARNING, "* SAX Warning: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static void
other_sax_error (void *data, const char *msg, ...)
{
    va_list args;
    char *tmp;

    va_start (args, msg);

    tmp = g_strdup_vprintf (msg, args);
    debug (DEBUG_LEVEL_ERROR, "* SAX Error: %s", tmp);
    g_free (tmp);

    va_end (args);
}

static xmlSAXHandler other_sax_handler = {
    NULL,      /* internalSubset */
    NULL,      /* isStandalone */
    NULL,      /* hasInternalSubset */
    NULL,      /* hasExternalSubset */
    NULL,      /* resolveEntity */
    NULL,      /* getEntity */
    NULL,      /* entityDecl */
    NULL,      /* notationDecl */
    NULL,      /* attributeDecl */
    NULL,      /* elementDecl */
    NULL,      /* unparsedEntityDecl */
    NULL,      /* setDocumentLocator */
    NULL,      /* startDocument */
    NULL,      /* endDocument */
    (startElementSAXFunc) other_sax_start_element, /* startElement */
    (endElementSAXFunc) other_sax_end_element,     /* endElement */
    NULL,      /* reference */
    (charactersSAXFunc) other_sax_characters,      /* characters */
    NULL,      /* ignorableWhitespace */
    NULL,      /* processingInstruction */
    NULL,      /* comment */
    other_sax_warning,      /* warning */
    other_sax_error,      /* error */
    other_sax_error,      /* fatalError */
};

void
yum_xml_parse_other (const char *filename,
                     PackageFn callback,
                     gpointer user_data)
{
    OtherSAXContext ctx;
    int rc;

    ctx.state = OTHER_PARSER_TOPLEVEL;
    ctx.callback = callback;
    ctx.user_data = user_data;
    ctx.current_package = NULL;
    ctx.current_entry = NULL;
    ctx.text_buffer = g_string_sized_new (PACKAGE_FIELD_SIZE);

    xmlSubstituteEntitiesDefault (1);
    rc = xmlSAXUserParseFile (&other_sax_handler, &ctx, filename);

    if (ctx.current_package) {
        debug (DEBUG_LEVEL_WARNING, "Incomplete package lost");
        package_free (ctx.current_package);
    }

    if (ctx.current_entry)
        g_free (ctx.current_entry);

    g_string_free (ctx.text_buffer, TRUE);
}
