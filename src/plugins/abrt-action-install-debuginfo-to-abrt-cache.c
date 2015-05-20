/*
    Copyright (C) 2011  ABRT Team
    Copyright (C) 2011  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "libabrt.h"

#define EXECUTABLE "abrt-action-install-debuginfo"

/* A binary wrapper is needed around python scripts if we want
 * to run them in sgid/suid mode.
 *
 * This is such a wrapper.
 */
int main(int argc, char **argv)
{
    /* I18n */
    setlocale(LC_ALL, "");
#if ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

    abrt_init(argv);

    /* Can't keep these strings/structs static: _() doesn't support that */
    const char *program_usage_string = _(
        "& [-y] [-i BUILD_IDS_FILE|-i -] [-e PATH[:PATH]...]\n"
        "\t[-r REPO]\n"
        "\n"
        "Installs debuginfo packages for all build-ids listed in BUILD_IDS_FILE to\n"
        "ABRT system cache."
    );

    enum {
        OPT_v = 1 << 0,
        OPT_y = 1 << 1,
        OPT_i = 1 << 2,
        OPT_e = 1 << 3,
        OPT_r = 1 << 4,
        OPT_s = 1 << 5,
    };

    const char *build_ids = "build_ids";
    const char *exact = NULL;
    const char *repo = NULL;
    const char *size_mb = NULL;

    struct options program_options[] = {
        OPT__VERBOSE(&g_verbose),
        OPT_BOOL  ('y', "yes",         NULL,                   _("Noninteractive, assume 'Yes' to all questions")),
        OPT_STRING('i', "ids",   &build_ids, "BUILD_IDS_FILE", _("- means STDIN, default: build_ids")),
        OPT_STRING('e', "exact",     &exact, "EXACT",          _("Download only specified files")),
        OPT_STRING('r', "repo",       &repo, "REPO",           _("Pattern to use when searching for repos, default: *debug*")),
        OPT_STRING('s', "size_mb", &size_mb, "SIZE_MB",        _("Ignored option")),
        OPT_END()
    };
    const unsigned opts = parse_opts(argc, argv, program_options, program_usage_string);

    const gid_t egid = getegid();
    const gid_t rgid = getgid();
    const uid_t euid = geteuid();
    const gid_t ruid = getuid();

    /* We need to open the build ids file under the caller's UID/GID to avoid
     * information disclosures when reading files with changed UID.
     * Unfortunately, we cannot replace STDIN with the new fd because ABRT uses
     * STDIN to communicate with the caller. So, the following code opens a
     * dummy file descriptor to the build ids file and passes the new fd's proc
     * path to the wrapped program in the ids argument.
     * The new fd remains opened, the OS will close it for us. */
    char *build_ids_self_fd = NULL;
    if (strcmp("-", build_ids) != 0)
    {
        if (setregid(egid, rgid) < 0)
            perror_msg_and_die("setregid(egid, rgid)");

        if (setreuid(euid, ruid) < 0)
            perror_msg_and_die("setreuid(euid, ruid)");

        const int build_ids_fd = open(build_ids, O_RDONLY);

        if (setregid(rgid, egid) < 0)
            perror_msg_and_die("setregid(rgid, egid)");

        if (setreuid(ruid, euid) < 0 )
            perror_msg_and_die("setreuid(ruid, euid)");

        if (build_ids_fd < 0)
            perror_msg_and_die("Failed to open file '%s'", build_ids);

        /* We are not going to free this memory. There is no place to do so. */
        build_ids_self_fd = xasprintf("/proc/self/fd/%d", build_ids_fd);
    }

    /* name, -v, --ids, -, -y, -e, EXACT, -r, REPO, --, NULL */
    const char *args[11];
    {
        const char *verbs[] = { "", "-v", "-vv", "-vvv" };
        unsigned i = 0;
        args[i++] = EXECUTABLE;
        args[i++] = "--ids";
        args[i++] = (build_ids_self_fd != NULL) ? build_ids_self_fd : "-";
        if (g_verbose > 0)
            args[i++] = verbs[g_verbose <= 3 ? g_verbose : 3];
        if ((opts & OPT_y))
            args[i++] = "-y";
        if ((opts & OPT_e))
        {
            args[i++] = "--exact";
            args[i++] = exact;
        }
        if ((opts & OPT_r))
        {
            args[i++] = "--repo";
            args[i++] = repo;
        }
        args[i++] = "--";
        args[i] = NULL;
    }

    /* Switch real user/group to effective ones.
     * Otherwise yum library gets confused - gets EPERM (why??).
     */
    /* do setregid only if we have to, to not upset selinux needlessly */
    if (egid != rgid)
        setregid(egid, egid);
    if (euid != ruid)
    {
        setreuid(euid, euid);
        /* We are suid'ed! */
        /* Prevent malicious user from messing up with suid'ed process: */
#if 1
// We forgot to sanitize PYTHONPATH. And who knows what else we forgot
// (especially considering *future* new variables of this kind).
// We switched to clearing entire environment instead:
        clearenv();
#else
        /* Clear dangerous stuff from env */
        static const char forbid[] =
            "LD_LIBRARY_PATH" "\0"
            "LD_PRELOAD" "\0"
            "LD_TRACE_LOADED_OBJECTS" "\0"
            "LD_BIND_NOW" "\0"
            "LD_AOUT_LIBRARY_PATH" "\0"
            "LD_AOUT_PRELOAD" "\0"
            "LD_NOWARN" "\0"
            "LD_KEEPDIR" "\0"
        ;
        const char *p = forbid;
        do {
            unsetenv(p);
            p += strlen(p) + 1;
        } while (*p);
#endif
        /* Set safe PATH */
// TODO: honor configure --prefix here by adding it to PATH
// (otherwise abrt-action-install-debuginfo would fail to spawn abrt-action-trim-files):
        if (euid == 0)
            putenv((char*) "PATH=/usr/sbin:/sbin:/usr/bin:/bin");
        else
            putenv((char*) "PATH=/usr/bin:/bin");

        /* Use safe umask */
        umask(0022);
    }

    execvp(EXECUTABLE, (char **)args);
    error_msg_and_die("Can't execute %s", EXECUTABLE);
}
