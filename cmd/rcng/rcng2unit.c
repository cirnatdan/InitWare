/*******************************************************************

        LICENCE NOTICE

These coded instructions, statements, and computer programs are part
of the  InitWare Suite of Middleware,  and  they are protected under
copyright law. They may not be distributed,  copied,  or used except
under the provisions of  the  terms  of  the  Library General Public
Licence version 2.1 or later, in the file "LICENSE.md", which should
have been included with this software

        Copyright Notice

    (c) 2021 David Mackay
        All rights reserved.

*********************************************************************/

#include <err.h>
#include <limits.h>
#include <stdio.h>

#include "install.h"
#include "mkdir.h"
#include "path-util.h"
#include "strv.h"

typedef struct RcNgService {
        const char *name;
        /* path to original rc script */
        const char *src_path;
        /*
         * All entries for the PROVIDE line. The first is usually identical to the
         * basename of the script, which is stored in @name. We therefore test
         * whether a provide entry is equal to @name before we generate a symlink.
         */
        char **provides;
        char **requires;
        char **before;
} RcNgService;

static int parse_rcscript(FILE *rcscript, RcNgService *svc) {
        int r;

        while (!feof(rcscript)) {
                char l[LINE_MAX], *t;

                if (!fgets(l, sizeof(l), rcscript)) {
                        if (feof(rcscript))
                                break;

                        log_error("Failed to read RC script '%s': %m", svc->src_path);
                        return -errno;
                }

                t = strstrip(l);
                if (*t != '#')
                        continue;

                t += 2;

                if (strneq(t, "PROVIDE:", 8))
                        svc->provides = strv_split(t + 9, " ");
                else if (strneq(t, "REQUIRE:", 8))
                        svc->requires = strv_split(t + 9, " ");
                else if (strneq(t, "BEFORE:", 7))
                        svc->before = strv_split(t + 8, " ");
        }

        return 0;
}

static int emit_name_list(FILE *out_f, char **names, bool append_svc) {
        char **el;

        STRV_FOREACH (el, names) {
                if (el != names) /* space before, except for first entry */
                        fputs(" ", out_f);
                if (append_svc) {
                        strextend(el, ".service", NULL);
                        if (!*el)
                                return log_oom();
                }
                fputs(*el, out_f);
        }

        return 0;
}

static int do_wanted_symlinks(const char *name, const char *out_name, const char *out_dir, char **wanted_bys) {
        char **wanted_by;
        int r;

        STRV_FOREACH (wanted_by, wanted_bys) {
                char *link;

                link = strjoin(out_dir, "/", *wanted_by, ".wants/", name, ".service", NULL);
                if (!link) {
                        r = log_oom();
                        goto finish;
                }

                mkdir_parents_label(link, 0755);
                r = symlink(out_name, link);
                if (r < 0) {
                        if (errno == EEXIST)
                                r = 0;
                        else
                                log_error(
                                        "Failed to create symlink with source %s named %s: %m; "
                                        "continuing with other symlinks.",
                                        out_name,
                                        link);
                }

                free(link);
        }

finish:
        free(link);
        return -r;
}

static int do_provides(const char *name, const char *out_name, const char *out_dir, char **provides) {
        char **provide;
        int r;

        STRV_FOREACH (provide, provides) {
                char *link;

                if (streq(*provide, name)) {
                        printf("Not symlinking default name %s.\n", name);
                        continue;
                }
                link = strjoin(out_dir, "/", *provide, ".service", NULL);
                if (!link)
                        return log_oom();

                r = symlink(out_name, link);
                if (r < 0) {
                        if (errno == EEXIST)
                                r = 0;
                        else
                                log_error(
                                        "Failed to create symlink with source %s named %s: %m; "
                                        "continuing with any other symlinks.",
                                        out_name,
                                        link);
                }
                free(link);
        }


        return 0;
}

static int emit_units(const char *out_dir, RcNgService *svc) {

        char *out_name;
        FILE *out_f;
        int r;

        out_name = strjoin(out_dir, "/", svc->name, ".service", NULL);

        if (!out_name) {
                r = log_oom();
                goto finish;
        }

        unlink(out_name);

        out_f = fopen(out_name, "wxe");

        if (!out_f) {
                log_error("Failed to open %s for writing: %m\n", out_name);
                return -errno;
        }

        fprintf(out_f,
                "# Automatically generated by the InitWare Mewburn RC Script Converter\n\n"
                "[Unit]\n"
                "Documentation=man:iw_rcng(8)\n"
                "SourcePath=%s\n",
                svc->src_path);

        if (svc->requires) {
                fprintf(out_f, "Wants="); /* we downgrade Requires. */
                r = emit_name_list(out_f, svc->requires, true);
                if (r < 0)
                        goto finish;
                fputs("\n", out_f);
                fprintf(out_f, "After=");
                r = emit_name_list(out_f, svc->requires, false);
                if (r < 0)
                        goto finish;
                fputs("\n", out_f);
        }

        if (svc->before) {
                fprintf(out_f, "Before=");
                r = emit_name_list(out_f, svc->before, true);
                if (r < 0)
                        goto finish;
                r = do_wanted_symlinks(svc->name, out_name, out_dir, svc->before);
                fputs("\n", out_f);
        }

        if (svc->provides) {
                r = do_provides(svc->name, out_name, out_dir, svc->provides);
                if (r < 0)
                        goto finish;
                fputs("\n", out_f);
        }

        fprintf(out_f,
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=" BinPath_Runrcng
                " %s start\n"
                "ExecStop=" BinPath_Runrcng " %s stop\n",
                svc->src_path,
                svc->src_path);

        fputc('\n', out_f);
finish:
        fclose(out_f);

        return r;
}

int main(int argc, char *argv[]) {
        FILE *rcscript;
        char retcode;
        const char *name;
        int r;
        RcNgService *svc;

        if (argc != 3)
                errx(EXIT_FAILURE, "Usage: %s /path/to/rc.d/service /path/to/out.service", argv[0]);

        rcscript = fopen(argv[1], "r");
        if (!rcscript)
                err(EXIT_FAILURE, "Failed to open RC script %s", argv[1]);

        name = path_get_file_name(argv[1]);
        printf("Converting RC script %s\n", name);

        svc = new0(RcNgService, 1);
        svc->name = name;
        svc->src_path = argv[1];

        r = parse_rcscript(rcscript, svc);
        if (r < 0) {
                log_error("Failed to parse RC script: %s\n", strerror(-r));
                goto finish;
        }

        r = emit_units(argv[2], svc);
        if (r < 0) {
                log_error("Failed to emit units for RC script: %s\n", strerror(-r));
        }

finish:
        strv_free(svc->before);
        strv_free(svc->provides);
        strv_free(svc->requires);
        free(svc);
        return -r;
}