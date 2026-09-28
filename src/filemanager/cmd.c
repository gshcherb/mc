/*
   Routines invoked by a function key
   They normally operate on the current panel.

   Copyright (C) 1994-2026
   Free Software Foundation, Inc.

   Written by:
   Andrew Borodin <aborodin@vmail.ru>, 2013-2022

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/** \file cmd.c
 *  \brief Source: routines invoked by a function key
 *
 *  They normally operate on the current panel.
 */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef ENABLE_VFS_NET
#include <netdb.h>
#endif
#include <unistd.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>

#include "lib/global.h"

#include "lib/tty/tty.h"  // LINES, tty_touch_screen()
#include "lib/tty/key.h"  // ALT() macro
#include "lib/mcconfig.h"
#include "lib/filehighlight.h"  // MC_FHL_INI_FILE
#include "lib/vfs/vfs.h"
#include "lib/fileloc.h"
#include "lib/strutil.h"
#include "lib/file-entry.h"
#include "lib/util.h"
#include "lib/widget.h"
#include "lib/widget/dialog-switch.h"  // top_dlg, filemanager
#include "lib/keybind.h"               // CK_Down, CK_History
#include "lib/event.h"    // mc_event_raise()

#include "src/setup.h"
#include "src/execute.h"  // toggle_panels()
#include "src/history.h"
#include "src/util.h"  // check_for_default(), file_error_message()

#include "src/viewer/mcviewer.h"

#ifdef USE_INTERNAL_EDIT
#include "src/editor/edit.h"
#endif

#ifdef USE_DIFF_VIEW
#include "src/diffviewer/ydiff.h"
#endif

#include "filegui.h"
#include "hotlist.h"      // hotlist_show()
#include "tree.h"         // tree_chdir()
#include "filemanager.h"  // change_panel()
#include "command.h"      // cmdline
#include "layout.h"       // get_current_type()
#include "ext.h"          // regex_command()
#include "boxes.h"        // cd_box()
#include "dir.h"
#include "cd.h"
#include "ioblksize.h"  // IO_BUFSIZE

#include "cmd.h"  // Our definitions

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

/*** file scope type declarations ****************************************************************/

enum CompareMode
{
    compare_quick = 0,
    compare_size_only,
    compare_thourough
};

/*** forward declarations (file scope functions) *************************************************/

static int adb_devices_timer_attention (int fd, void *data);
static void adb_devices_timer_arm (void);

/*** file scope variables ************************************************************************/

#ifdef ENABLE_VFS_NET
static const char *machine_str = N_ ("Enter machine name (F1 for details):");
#endif

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */
/**
 * Run viewer (internal or external) on the current file.
 * If @plain_view is TRUE, force internal viewer and raw mode (used for F13).
 */
static void
do_view_cmd (WPanel *panel, gboolean plain_view)
{
    const file_entry_t *fe;

    fe = panel_current_entry (panel);
    if (fe == NULL)
        return;

    // Directories are viewed by changing to them
    if (S_ISDIR (fe->st.st_mode) || link_isdir (fe))
    {
        vfs_path_t *fname_vpath;

        if (confirm_view_dir && (panel->marked != 0 || panel->dirs_marked != 0)
            && query_dialog (_ ("Confirmation"), _ ("Files tagged, want to cd?"), D_NORMAL, 2,
                             _ ("&Yes"), _ ("&No"))
                != 0)
            return;

        fname_vpath = vfs_path_from_str (fe->fname->str);
        if (!panel_cd (panel, fname_vpath, cd_exact))
            cd_error_message (fe->fname->str);
        vfs_path_free (fname_vpath, TRUE);
    }
    else
    {
        vfs_path_t *filename_vpath;

        filename_vpath = vfs_path_from_str (fe->fname->str);
        view_file (filename_vpath, plain_view, use_internal_view);
        vfs_path_free (filename_vpath, TRUE);
    }

    repaint_screen ();
}

/* --------------------------------------------------------------------------------------------- */

static inline void
do_edit (const vfs_path_t *what_vpath)
{
    edit_file_at_line (what_vpath, use_internal_edit, 0);
}

/* --------------------------------------------------------------------------------------------- */

static int
compare_files (const vfs_path_t *vpath1, const vfs_path_t *vpath2, off_t size)
{
    int file1;
    int result = -1;  // Different by default

    if (size == 0)
        return 0;

    file1 = open (vfs_path_as_str (vpath1), O_RDONLY);
    if (file1 >= 0)
    {
        int file2;

        file2 = open (vfs_path_as_str (vpath2), O_RDONLY);
        if (file2 >= 0)
        {
            char buf1[IO_BUFSIZE], buf2[IO_BUFSIZE];
            ssize_t n1, n2;

            rotate_dash (TRUE);
            do
            {
                while ((n1 = read (file1, buf1, sizeof (buf1))) == -1 && errno == EINTR)
                    ;
                while ((n2 = read (file2, buf2, sizeof (buf2))) == -1 && errno == EINTR)
                    ;
            }
            while (n1 == n2 && n1 == sizeof (buf1) && memcmp (buf1, buf2, sizeof (buf1)) == 0);
            result = (n1 != n2) || (memcmp (buf1, buf2, MIN ((size_t) n1, sizeof (buf1))) != 0);
            rotate_dash (FALSE);

            close (file2);
        }
        close (file1);
    }

    return result;
}

/* --------------------------------------------------------------------------------------------- */

static void
compare_dir (WPanel *panel, const WPanel *other, enum CompareMode mode)
{
    int i, j;

    // No marks by default
    panel->marked = 0;
    panel->total = 0;
    panel->dirs_marked = 0;

    // Handle all files in the panel
    for (i = 0; i < panel->dir.len; i++)
    {
        file_entry_t *source = &panel->dir.list[i];
        const char *source_fname;

        // Default: unmarked
        file_mark (panel, i, 0);

        // Skip directories
        if (S_ISDIR (source->st.st_mode))
            continue;

        source_fname = source->fname->str;
        if (panel->is_panelized)
            source_fname = x_basename (source_fname);

        // Search the corresponding entry from the other panel
        for (j = 0; j < other->dir.len; j++)
        {
            const char *other_fname;

            other_fname = other->dir.list[j].fname->str;
            if (other->is_panelized)
                other_fname = x_basename (other_fname);

            if (strcmp (source_fname, other_fname) == 0)
                break;
        }

        if (j >= other->dir.len)
            // Not found -> mark
            do_file_mark (panel, i, 1);
        else
        {
            // Found
            file_entry_t *target = &other->dir.list[j];

            if (mode != compare_size_only)
                // Older version is not marked
                if (source->st.st_mtime < target->st.st_mtime)
                    continue;

            // Newer version with different size is marked
            if (source->st.st_size != target->st.st_size)
            {
                do_file_mark (panel, i, 1);
                continue;
            }

            if (mode == compare_size_only)
                continue;

            if (mode == compare_quick)
            {
                // Thorough compare off, compare only time stamps
                // Mark newer version, don't mark version with the same date
                if (source->st.st_mtime > target->st.st_mtime)
                    do_file_mark (panel, i, 1);

                continue;
            }

            // Thorough compare on, do byte-by-byte comparison
            {
                vfs_path_t *src_name, *dst_name;

                src_name =
                    vfs_path_append_new (panel->cwd_vpath, source->fname->str, (char *) NULL);
                dst_name =
                    vfs_path_append_new (other->cwd_vpath, target->fname->str, (char *) NULL);
                if (compare_files (src_name, dst_name, source->st.st_size))
                    do_file_mark (panel, i, 1);
                vfs_path_free (src_name, TRUE);
                vfs_path_free (dst_name, TRUE);
            }
        }
    }  // for (i ...)
}

/* --------------------------------------------------------------------------------------------- */

static void
do_link (link_type_t link_type, const char *fname)
{
    char *dest = NULL, *src = NULL;
    vfs_path_t *dest_vpath = NULL;

    if (link_type == LINK_HARDLINK)
    {
        vfs_path_t *fname_vpath;

        src = g_strdup_printf (_ ("Link %s to:"), str_trunc (fname, 46));
        dest =
            input_expand_dialog (_ ("Link"), src, MC_HISTORY_FM_LINK, "", INPUT_COMPLETE_FILENAMES);
        if (dest == NULL || *dest == '\0')
            goto cleanup;

        save_cwds_stat ();

        fname_vpath = vfs_path_from_str (fname);
        dest_vpath = vfs_path_from_str (dest);
        if (mc_link (fname_vpath, dest_vpath) == -1)
            file_error_message (_ ("Cannot create link\n%s"), dest);
        vfs_path_free (fname_vpath, TRUE);
    }
    else
    {
        vfs_path_t *s, *d;

        /* suggest the full path for symlink, and either the full or
           relative path to the file it points to  */
        s = vfs_path_append_new (current_panel->cwd_vpath, fname, (char *) NULL);

        if (get_other_type () == view_listing)
            d = vfs_path_append_new (other_panel->cwd_vpath, fname, (char *) NULL);
        else
            d = vfs_path_from_str (fname);

        if (link_type == LINK_SYMLINK_RELATIVE)
        {
            char *s_str;

            s_str = diff_two_paths (other_panel->cwd_vpath, s);
            vfs_path_free (s, TRUE);
            s = vfs_path_from_str_flags (s_str, VPF_NO_CANON);
            g_free (s_str);
        }

        symlink_box (s, d, &dest, &src);
        vfs_path_free (d, TRUE);
        vfs_path_free (s, TRUE);

        if (dest == NULL || *dest == '\0' || src == NULL || *src == '\0')
            goto cleanup;

        save_cwds_stat ();

        dest_vpath = vfs_path_from_str_flags (dest, VPF_NO_CANON);

        s = vfs_path_from_str (src);
        if (mc_symlink (dest_vpath, s) == -1)
            file_error_message (_ ("Cannot create symbolic link\n%s"), dest);
        vfs_path_free (s, TRUE);
    }

    update_panels (UP_OPTIMIZE, UP_KEEPSEL);
    repaint_screen ();

cleanup:
    vfs_path_free (dest_vpath, TRUE);
    g_free (src);
    g_free (dest);
}

/* --------------------------------------------------------------------------------------------- */

#if defined(ENABLE_VFS_NET)
static void
nice_cd (const char *text, const char *xtext, const char *help, const char *history_name,
         const char *prefix, int to_home, gboolean strip_password)
{
    char *machine;
    char *cd_path;

    machine = input_dialog_help (text, xtext, help, history_name, INPUT_LAST_TEXT, strip_password,
                                 INPUT_COMPLETE_FILENAMES | INPUT_COMPLETE_CD
                                     | INPUT_COMPLETE_HOSTNAMES | INPUT_COMPLETE_USERNAMES);
    if (machine == NULL)
        return;

    to_home = 0; /* FIXME: how to solve going to home nicely? /~/ is
                    ugly as hell and leads to problems in vfs layer */

    if (strncmp (prefix, machine, strlen (prefix)) == 0)
        cd_path = g_strconcat (machine, to_home ? "/~/" : (char *) NULL, (char *) NULL);
    else
        cd_path = g_strconcat (prefix, machine, to_home ? "/~/" : (char *) NULL, (char *) NULL);

    g_free (machine);

    if (!IS_PATH_SEP (*cd_path))
    {
        char *tmp = cd_path;

        cd_path = g_strconcat (PATH_SEP_STR, tmp, (char *) NULL);
        g_free (tmp);
    }

    {
        panel_view_mode_t save_type;
        vfs_path_t *cd_vpath;

        save_type = get_panel_type (MENU_PANEL_IDX);

        if (save_type != view_listing)
            create_panel (MENU_PANEL_IDX, view_listing);

        cd_vpath = vfs_path_from_str_flags (cd_path, VPF_NO_CANON);
        if (!panel_do_cd (MENU_PANEL, cd_vpath, cd_parse_command))
        {
            cd_error_message (cd_path);

            if (save_type != view_listing)
                create_panel (MENU_PANEL_IDX, save_type);
        }
        vfs_path_free (cd_vpath, TRUE);
    }
    g_free (cd_path);

    // In case of passive panel, restore current VFS directory that was changed in panel_do_cd()
    if (MENU_PANEL != current_panel)
        (void) mc_chdir (current_panel->cwd_vpath);
}
#endif

/* --------------------------------------------------------------------------------------------- */

static void
configure_panel_listing (WPanel *p, int list_format, int brief_cols, gboolean use_msformat,
                         char **user, char **status)
{
    p->user_mini_status = use_msformat;
    p->list_format = list_format;

    if (list_format == list_brief)
        p->brief_cols = brief_cols;

    if (list_format == list_user || use_msformat)
    {
        g_free (p->user_format);
        p->user_format = *user;
        *user = NULL;

        g_free (p->user_status_format[list_format]);
        p->user_status_format[list_format] = *status;
        *status = NULL;

        set_panel_formats (p);
    }

    set_panel_formats (p);
    do_refresh ();
}

/* --------------------------------------------------------------------------------------------- */

static void
switch_to_listing (int panel_index)
{
    if (get_panel_type (panel_index) != view_listing)
        create_panel (panel_index, view_listing);
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

gboolean
view_file_at_line (const vfs_path_t *filename_vpath, gboolean plain_view, gboolean internal,
                   long start_line, off_t search_start, off_t search_end)
{
    gboolean ret = TRUE;

    if (plain_view)
    {
        mcview_mode_flags_t changed_flags;

        mcview_clear_mode_flags (&changed_flags);
        mcview_altered_flags.hex = FALSE;
        mcview_altered_flags.magic = FALSE;
        mcview_altered_flags.nroff = FALSE;
        if (mcview_global_flags.hex)
            changed_flags.hex = TRUE;
        if (mcview_global_flags.magic)
            changed_flags.magic = TRUE;
        if (mcview_global_flags.nroff)
            changed_flags.nroff = TRUE;
        mcview_global_flags.hex = FALSE;
        mcview_global_flags.magic = FALSE;
        mcview_global_flags.nroff = FALSE;

        ret = mcview_viewer (NULL, filename_vpath, start_line, search_start, search_end);

        if (changed_flags.hex && !mcview_altered_flags.hex)
            mcview_global_flags.hex = TRUE;
        if (changed_flags.magic && !mcview_altered_flags.magic)
            mcview_global_flags.magic = TRUE;
        if (changed_flags.nroff && !mcview_altered_flags.nroff)
            mcview_global_flags.nroff = TRUE;

        dialog_switch_process_pending ();
    }
    else if (internal)
    {
        char view_entry[BUF_TINY];

        if (start_line > 0)
            g_snprintf (view_entry, sizeof (view_entry), "View:%ld", start_line);
        else
            strcpy (view_entry, "View");

        ret = (regex_command (filename_vpath, view_entry) == 0);
        if (ret)
        {
            ret = mcview_viewer (NULL, filename_vpath, start_line, search_start, search_end);
            dialog_switch_process_pending ();
        }
    }
    else
    {
        static const char *viewer = NULL;

        if (viewer == NULL)
        {
            viewer = getenv ("VIEWER");
            if (viewer == NULL)
                viewer = getenv ("PAGER");
            if (viewer == NULL)
                viewer = "view";
        }

        execute_external_editor_or_viewer (viewer, filename_vpath, start_line);
    }

    return ret;
}

/* --------------------------------------------------------------------------------------------- */
/** view_file (filename, plain_view, internal)
 *
 * Inputs:
 *   filename_vpath: The file name to view
 *   plain_view:     If set does not do any fancy pre-processing (no filtering) and
 *                   always invokes the internal viewer.
 *   internal:       If set uses the internal viewer, otherwise an external viewer.
 */

gboolean
view_file (const vfs_path_t *filename_vpath, gboolean plain_view, gboolean internal)
{
    return view_file_at_line (filename_vpath, plain_view, internal, 0, 0, 0);
}

/* --------------------------------------------------------------------------------------------- */
/** Run user's preferred viewer on the current file */

void
view_cmd (WPanel *panel)
{
    do_view_cmd (panel, FALSE);
}

/* --------------------------------------------------------------------------------------------- */
/** Ask for file and run user's preferred viewer on it */

void
view_file_cmd (const WPanel *panel)
{
    const file_entry_t *fe;
    char *filename;
    vfs_path_t *vpath;

    fe = panel_current_entry (panel);
    if (fe == NULL)
        return;

    filename = input_expand_dialog (_ ("View file"), _ ("Filename:"), MC_HISTORY_FM_VIEW_FILE,
                                    fe->fname->str, INPUT_COMPLETE_FILENAMES);
    if (filename == NULL)
        return;

    vpath = vfs_path_from_str (filename);
    g_free (filename);
    view_file (vpath, FALSE, use_internal_view);
    vfs_path_free (vpath, TRUE);
}

/* --------------------------------------------------------------------------------------------- */
/** Run plain internal viewer on the current file */
void
view_raw_cmd (WPanel *panel)
{
    do_view_cmd (panel, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
view_filtered_cmd (const WPanel *panel)
{
    char *command;
    const char *initial_command;

    if (input_is_empty (cmdline))
    {
        const file_entry_t *fe;

        fe = panel_current_entry (panel);
        if (fe == NULL)
            return;

        initial_command = fe->fname->str;
    }
    else
        initial_command = input_get_ctext (cmdline);

    command = input_dialog (_ ("Filtered view"), _ ("Filter command and arguments:"),
                            MC_HISTORY_FM_FILTERED_VIEW, initial_command,
                            INPUT_COMPLETE_FILENAMES | INPUT_COMPLETE_COMMANDS);

    if (command != NULL)
    {
        mcview_viewer (command, NULL, 0, 0, 0);
        g_free (command);
        dialog_switch_process_pending ();
    }
}

/* --------------------------------------------------------------------------------------------- */

void
edit_file_at_line (const vfs_path_t *what_vpath, gboolean internal, long start_line)
{
#ifdef USE_INTERNAL_EDIT
    if (internal)
    {
        const edit_arg_t arg = { (vfs_path_t *) what_vpath, start_line };

        edit_file (&arg);
        dialog_switch_process_pending ();
    }
    else
#endif
    {
        static const char *editor = NULL;

        (void) internal;

        if (editor == NULL)
        {
            editor = getenv ("EDITOR");
            if (editor == NULL)
                editor = get_default_editor ();
        }

        execute_external_editor_or_viewer (editor, what_vpath, start_line);
    }
}

/* --------------------------------------------------------------------------------------------- */

void
edit_cmd (const WPanel *panel)
{
    const file_entry_t *fe;
    vfs_path_t *fname;

    fe = panel_current_entry (panel);
    if (fe == NULL)
        return;

    fname = vfs_path_from_str (fe->fname->str);
    if (regex_command (fname, "Edit") == 0)
        do_edit (fname);
    vfs_path_free (fname, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

#ifdef USE_INTERNAL_EDIT
void
edit_cmd_force_internal (const WPanel *panel)
{
    const file_entry_t *fe;
    vfs_path_t *fname;

    fe = panel_current_entry (panel);
    if (fe == NULL)
        return;

    fname = vfs_path_from_str (fe->fname->str);
    if (regex_command (fname, "Edit") == 0)
        edit_file_at_line (fname, TRUE, 1);
    vfs_path_free (fname, TRUE);
}
#endif

/* --------------------------------------------------------------------------------------------- */

void
edit_cmd_new (void)
{
    vfs_path_t *fname_vpath = NULL;

    if (editor_ask_filename_before_edit)
    {
        char *fname;

        fname = input_expand_dialog (_ ("Edit file"), _ ("Enter file name:"), MC_HISTORY_EDIT_LOAD,
                                     "", INPUT_COMPLETE_FILENAMES);
        if (fname == NULL)
            return;

        if (*fname != '\0')
            fname_vpath = vfs_path_from_str (fname);

        g_free (fname);
    }

    mc_global.source_codepage = default_source_codepage;
    do_edit (fname_vpath);

    vfs_path_free (fname_vpath, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
mkdir_cmd (WPanel *panel)
{
    const file_entry_t *fe;
    char *dir;
    const char *name = "";

    fe = panel_current_entry (panel);
    if (fe == NULL)
        return;

    // If 'on' then automatically fills name with current item name
    if (auto_fill_mkdir_name && !DIR_IS_DOTDOT (fe->fname->str))
        name = fe->fname->str;

    dir = input_expand_dialog (_ ("Create a new Directory"), _ ("Enter directory name:"),
                               MC_HISTORY_FM_MKDIR, name, INPUT_COMPLETE_FILENAMES);

    if (dir != NULL && *dir != '\0')
    {
        vfs_path_t *absdir;

        if (IS_PATH_SEP (dir[0]) || dir[0] == '~')
            absdir = vfs_path_from_str (dir);
        else
        {
            // possible escaped '~'
            // allow create directory with name '~'
            char *tmpdir = dir;

            if (dir[0] == '\\' && dir[1] == '~')
                tmpdir = dir + 1;

            absdir = vfs_path_append_new (panel->cwd_vpath, tmpdir, (char *) NULL);
        }

        save_cwds_stat ();

        if (mc_mkdir (absdir, 0777) != 0)
            file_error_message (_ ("Cannot create directory\n%s"), vfs_path_as_str (absdir));
        else
        {
            update_panels (UP_OPTIMIZE, dir);
            repaint_screen ();
            select_item (panel);
        }

        vfs_path_free (absdir, TRUE);
    }
    g_free (dir);
}

/* --------------------------------------------------------------------------------------------- */

void
reread_cmd (void)
{
    panel_update_flags_t flag = UP_ONLY_CURRENT;

    if (get_current_type () == view_listing && get_other_type () == view_listing
        && vfs_path_equal (current_panel->cwd_vpath, other_panel->cwd_vpath))
        flag = UP_OPTIMIZE;

    update_panels (UP_RELOAD | flag, UP_KEEPSEL);
    repaint_screen ();
}

/* --------------------------------------------------------------------------------------------- */

void
ext_cmd (void)
{
    vfs_path_t *extdir_vpath;
    int dir = 0;

    if (geteuid () == 0)
        dir = query_dialog (_ ("Extension file edit"), _ ("Which extension file you want to edit?"),
                            D_NORMAL, 2, _ ("&User"), _ ("&System Wide"));

    extdir_vpath = vfs_path_build_filename (mc_global.sysconfig_dir, MC_EXT_FILE, (char *) NULL);

    if (dir == 0)
    {
        vfs_path_t *buffer_vpath;

        buffer_vpath = mc_config_get_full_vpath (MC_EXT_FILE);
        check_for_default (extdir_vpath, buffer_vpath);
        do_edit (buffer_vpath);
        vfs_path_free (buffer_vpath, TRUE);
    }
    else if (dir == 1)
    {
        if (!exist_file (vfs_path_get_last_path_str (extdir_vpath)))
        {
            vfs_path_free (extdir_vpath, TRUE);
            extdir_vpath =
                vfs_path_build_filename (mc_global.share_data_dir, MC_EXT_FILE, (char *) NULL);
        }
        do_edit (extdir_vpath);
    }

    vfs_path_free (extdir_vpath, TRUE);
    flush_extension_file ();
}

/* --------------------------------------------------------------------------------------------- */
/** edit file menu for mc */

void
edit_mc_menu_cmd (void)
{
    vfs_path_t *buffer_vpath;
    vfs_path_t *menufile_vpath;
    int dir = 0;

    query_set_sel (1);
    dir = query_dialog (_ ("Menu edit"), _ ("Which menu file do you want to edit?"), D_NORMAL,
                        geteuid () ? 2 : 3, _ ("&Local"), _ ("&User"), _ ("&System Wide"));

    menufile_vpath =
        vfs_path_build_filename (mc_global.sysconfig_dir, MC_GLOBAL_MENU, (char *) NULL);

    if (!exist_file (vfs_path_get_last_path_str (menufile_vpath)))
    {
        vfs_path_free (menufile_vpath, TRUE);
        menufile_vpath =
            vfs_path_build_filename (mc_global.share_data_dir, MC_GLOBAL_MENU, (char *) NULL);
    }

    switch (dir)
    {
    case 0:
        buffer_vpath = vfs_path_from_str (MC_LOCAL_MENU);
        check_for_default (menufile_vpath, buffer_vpath);
        chmod (vfs_path_get_last_path_str (buffer_vpath), 0600);
        break;

    case 1:
        buffer_vpath = mc_config_get_full_vpath (MC_USERMENU_FILE);
        check_for_default (menufile_vpath, buffer_vpath);
        break;

    case 2:
        buffer_vpath =
            vfs_path_build_filename (mc_global.sysconfig_dir, MC_GLOBAL_MENU, (char *) NULL);
        if (!exist_file (vfs_path_get_last_path_str (buffer_vpath)))
        {
            vfs_path_free (buffer_vpath, TRUE);
            buffer_vpath =
                vfs_path_build_filename (mc_global.share_data_dir, MC_GLOBAL_MENU, (char *) NULL);
        }
        break;

    default:
        vfs_path_free (menufile_vpath, TRUE);
        return;
    }

    do_edit (buffer_vpath);

    vfs_path_free (buffer_vpath, TRUE);
    vfs_path_free (menufile_vpath, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
edit_fhl_cmd (void)
{
    vfs_path_t *fhlfile_vpath = NULL;
    int dir = 0;

    if (geteuid () == 0)
        dir = query_dialog (_ ("Highlighting groups file edit"),
                            _ ("Which highlighting file you want to edit?"), D_NORMAL, 2,
                            _ ("&User"), _ ("&System Wide"));

    fhlfile_vpath =
        vfs_path_build_filename (mc_global.sysconfig_dir, MC_FHL_INI_FILE, (char *) NULL);

    if (dir == 0)
    {
        vfs_path_t *buffer_vpath;

        buffer_vpath = mc_config_get_full_vpath (MC_FHL_INI_FILE);
        check_for_default (fhlfile_vpath, buffer_vpath);
        do_edit (buffer_vpath);
        vfs_path_free (buffer_vpath, TRUE);
    }
    else if (dir == 1)
    {
        if (!exist_file (vfs_path_get_last_path_str (fhlfile_vpath)))
        {
            vfs_path_free (fhlfile_vpath, TRUE);
            fhlfile_vpath =
                vfs_path_build_filename (mc_global.sysconfig_dir, MC_FHL_INI_FILE, (char *) NULL);
        }
        do_edit (fhlfile_vpath);
    }

    vfs_path_free (fhlfile_vpath, TRUE);
    // refresh highlighting rules
    mc_fhl_free (&mc_filehighlight);
    mc_filehighlight = mc_fhl_new (TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
hotlist_cmd (WPanel *panel)
{
    char *target;

    target = hotlist_show (LIST_HOTLIST, panel);
    if (target == NULL)
        return;

    if (get_current_type () == view_tree)
    {
        vfs_path_t *vpath;

        vpath = vfs_path_from_str (target);
        tree_chdir (the_tree, vpath);
        vfs_path_free (vpath, TRUE);
    }
    else
    {
        vfs_path_t *deprecated_vpath;
        const char *deprecated_path;

        deprecated_vpath = vfs_path_from_str_flags (target, VPF_USE_DEPRECATED_PARSER);
        deprecated_path = vfs_path_as_str (deprecated_vpath);
        cd_to (deprecated_path);
        vfs_path_free (deprecated_vpath, TRUE);
    }

    g_free (target);
}

/* --------------------------------------------------------------------------------------------- */

#ifdef ENABLE_VFS
void
vfs_list (WPanel *panel)
{
    char *target;
    vfs_path_t *target_vpath;

    target = hotlist_show (LIST_VFSLIST, panel);
    if (target == NULL)
        return;

    target_vpath = vfs_path_from_str (target);
    if (!panel_cd (current_panel, target_vpath, cd_exact))
        cd_error_message (target);
    vfs_path_free (target_vpath, TRUE);
    g_free (target);
}
#endif

/* --------------------------------------------------------------------------------------------- */

void
compare_dirs_cmd (void)
{
    int choice;
    enum CompareMode thorough_flag;

    choice = query_dialog (_ ("Compare directories"), _ ("Select compare method:"), D_NORMAL, 4,
                           _ ("&Quick"), _ ("&Size only"), _ ("&Thorough"), _ ("&Cancel"));

    if (choice < 0 || choice > 2)
        return;

    thorough_flag = choice;

    if (get_current_type () == view_listing && get_other_type () == view_listing)
    {
        compare_dir (current_panel, other_panel, thorough_flag);
        compare_dir (other_panel, current_panel, thorough_flag);
    }
    else
        message (D_ERROR, MSG_ERROR,
                 _ ("Both panels should be in the listing mode\nto use this command"));
}

/* --------------------------------------------------------------------------------------------- */

#ifdef USE_DIFF_VIEW
void
diff_view_cmd (void)
{
    // both panels must be in the list mode
    if (get_current_type () == view_listing && get_other_type () == view_listing)
    {
        if (get_current_index () == 0)
            dview_diff_cmd (current_panel, other_panel);
        else
            dview_diff_cmd (other_panel, current_panel);

        if (mc_global.mc_run_mode == MC_RUN_FULL)
            update_panels (UP_OPTIMIZE, UP_KEEPSEL);

        dialog_switch_process_pending ();
    }
}
#endif

/* --------------------------------------------------------------------------------------------- */

void
swap_cmd (void)
{
    swap_panels ();
    tty_touch_screen ();
    repaint_screen ();
}

/* --------------------------------------------------------------------------------------------- */

void
link_cmd (link_type_t link_type)
{
    const file_entry_t *fe;

    fe = panel_current_entry (current_panel);
    if (fe != NULL)
        do_link (link_type, fe->fname->str);
}

/* --------------------------------------------------------------------------------------------- */

void
edit_symlink_cmd (void)
{
    const file_entry_t *fe;
    const char *p;

    fe = panel_current_entry (current_panel);
    if (fe == NULL)
        return;

    p = fe->fname->str;

    if (!S_ISLNK (fe->st.st_mode))
        message (D_ERROR, MSG_ERROR, _ ("'%s' is not a symbolic link"), p);
    else
    {
        char buffer[MC_MAXPATHLEN];
        int i;

        i = readlink (p, buffer, sizeof (buffer) - 1);
        if (i > 0)
        {
            char *q, *dest;

            buffer[i] = '\0';

            q = g_strdup_printf (_ ("Symlink '%s\' points to:"), str_trunc (p, 32));
            dest = input_expand_dialog (_ ("Edit symlink"), q, MC_HISTORY_FM_EDIT_LINK, buffer,
                                        INPUT_COMPLETE_FILENAMES);
            g_free (q);

            if (dest != NULL && *dest != '\0' && strcmp (buffer, dest) != 0)
            {
                vfs_path_t *p_vpath;

                p_vpath = vfs_path_from_str (p);

                save_cwds_stat ();

                if (mc_unlink (p_vpath) == -1)
                    file_error_message (_ ("Edit symlink, unable to remove\n%s"), p);
                else
                {
                    vfs_path_t *dest_vpath;

                    dest_vpath = vfs_path_from_str_flags (dest, VPF_NO_CANON);
                    if (mc_symlink (dest_vpath, p_vpath) == -1)
                        file_error_message (_ ("Cannot edit symlink\n%s"),
                                            vfs_path_as_str (dest_vpath));
                    vfs_path_free (dest_vpath, TRUE);
                }

                vfs_path_free (p_vpath, TRUE);

                update_panels (UP_OPTIMIZE, UP_KEEPSEL);
                repaint_screen ();
            }

            g_free (dest);
        }
    }
}

/* --------------------------------------------------------------------------------------------- */

void
help_cmd (void)
{
    ev_help_t event_data = { NULL, NULL };

    if (current_panel->quick_search.active)
        event_data.node = "[Quick search]";
    else
        event_data.node = "[main]";

    mc_event_raise (MCEVENT_GROUP_CORE, "help", &event_data);
}

/* --------------------------------------------------------------------------------------------- */

#ifdef ENABLE_VFS_FTP
void
ftplink_cmd (void)
{
    nice_cd (_ ("FTP to machine"), _ (machine_str), "[FTP File System]",
             ":ftplink_cmd: FTP to machine ", "ftp://", 1, TRUE);
}
#endif

/* --------------------------------------------------------------------------------------------- */

#ifdef ENABLE_VFS_SFTP
void
sftplink_cmd (void)
{
    nice_cd (_ ("SFTP to machine"), _ (machine_str),
             "[SFTP (SSH File Transfer Protocol) filesystem]", ":sftplink_cmd: SFTP to machine ",
             "sftp://", 1, TRUE);
}
#endif

/* --------------------------------------------------------------------------------------------- */

#ifdef ENABLE_VFS_SHELL
void
shelllink_cmd (void)
{
    nice_cd (_ ("Shell link to machine"), _ (machine_str), "[FIle transfer over SHell filesystem]",
             ":fishlink_cmd: Shell link to machine ", "sh://", 1, TRUE);
}
#endif

/* --------------------------------------------------------------------------------------------- */

void
quick_cd_cmd (WPanel *panel)
{
    char *p;

    p = cd_box (panel);
    if (p != NULL && *p != '\0')
        cd_to (p);
    g_free (p);
}

/* --------------------------------------------------------------------------------------------- */
/*!
   \brief calculate dirs sizes

   calculate dirs sizes and resort panel:
   dirs_selected = show size for selected dirs,
   otherwise = show size for dir under cursor:
   dir under cursor ".." = show size for all dirs,
   otherwise = show size for dir under cursor
 */

void
smart_dirsize_cmd (WPanel *panel)
{
    const file_entry_t *entry;

    entry = panel_current_entry (panel);
    if ((entry != NULL && S_ISDIR (entry->st.st_mode) && DIR_IS_DOTDOT (entry->fname->str))
        || panel->dirs_marked)
        dirsizes_cmd (panel);
    else
        single_dirsize_cmd (panel);
}

/* --------------------------------------------------------------------------------------------- */

void
single_dirsize_cmd (WPanel *panel)
{
    file_entry_t *entry;

    entry = panel_current_entry (panel);

    if (entry != NULL && S_ISDIR (entry->st.st_mode) && !DIR_IS_DOTDOT (entry->fname->str))
    {
        size_t dir_count = 0;
        size_t count = 0;
        uintmax_t total = 0;
        dirsize_status_msg_t dsm;
        vfs_path_t *p;

        p = vfs_path_from_str (entry->fname->str);

        memset (&dsm, 0, sizeof (dsm));
        status_msg_init (STATUS_MSG (&dsm), _ ("Directory scanning"), 0, dirsize_status_init_cb,
                         dirsize_status_update_cb, dirsize_status_deinit_cb);

        if (compute_dir_size (p, &dsm, &dir_count, &count, &total, FALSE) == FILE_CONT)
        {
            entry->st.st_size = (off_t) total;
            entry->f.dir_size_computed = 1;
        }

        vfs_path_free (p, TRUE);

        status_msg_deinit (STATUS_MSG (&dsm));
    }

    if (panels_options.mark_moves_down)
        send_message (panel, NULL, MSG_ACTION, CK_Down, NULL);

    recalculate_panel_summary (panel);

    if (panel->sort_field->sort_routine == (GCompareFunc) sort_size)
        panel_re_sort (panel);

    panel->dirty = TRUE;
}

/* --------------------------------------------------------------------------------------------- */

void
dirsizes_cmd (WPanel *panel)
{
    int i;
    dirsize_status_msg_t dsm;

    memset (&dsm, 0, sizeof (dsm));
    status_msg_init (STATUS_MSG (&dsm), _ ("Directory scanning"), 0, dirsize_status_init_cb,
                     dirsize_status_update_cb, dirsize_status_deinit_cb);

    for (i = 0; i < panel->dir.len; i++)
        if (S_ISDIR (panel->dir.list[i].st.st_mode)
            && ((panel->dirs_marked != 0 && panel->dir.list[i].f.marked != 0)
                || panel->dirs_marked == 0)
            && !DIR_IS_DOTDOT (panel->dir.list[i].fname->str))
        {
            vfs_path_t *p;
            size_t dir_count = 0;
            size_t count = 0;
            uintmax_t total = 0;
            gboolean ok;

            p = vfs_path_from_str (panel->dir.list[i].fname->str);
            ok = compute_dir_size (p, &dsm, &dir_count, &count, &total, FALSE) != FILE_CONT;
            vfs_path_free (p, TRUE);
            if (ok)
                break;

            panel->dir.list[i].st.st_size = (off_t) total;
            panel->dir.list[i].f.dir_size_computed = 1;
        }

    status_msg_deinit (STATUS_MSG (&dsm));

    recalculate_panel_summary (panel);

    if (panel->sort_field->sort_routine == (GCompareFunc) sort_size)
        panel_re_sort (panel);

    panel->dirty = TRUE;
}

/* --------------------------------------------------------------------------------------------- */

void
save_setup_cmd (void)
{
    vfs_path_t *vpath;
    const char *path;

    vpath = vfs_path_from_str_flags (mc_config_get_path (), VPF_STRIP_HOME);
    path = vfs_path_as_str (vpath);

    if (save_setup (TRUE, TRUE))
        message (D_NORMAL, _ ("Setup"), _ ("Setup saved to %s"), path);
    else
        message (D_ERROR, _ ("Setup"), _ ("Unable to save setup to %s"), path);

    vfs_path_free (vpath, TRUE);
}

/* --------------------------------------------------------------------------------------------- */

void
info_cmd_no_menu (void)
{
    if (get_panel_type (0) == view_info)
        create_panel (0, view_listing);
    else if (get_panel_type (1) == view_info)
        create_panel (1, view_listing);
    else
        create_panel (current_panel == left_panel ? 1 : 0, view_info);
}

/* --------------------------------------------------------------------------------------------- */

void
quick_cmd_no_menu (void)
{
    if (get_panel_type (0) == view_quick)
        create_panel (0, view_listing);
    else if (get_panel_type (1) == view_quick)
        create_panel (1, view_listing);
    else
        create_panel (current_panel == left_panel ? 1 : 0, view_quick);
}

/* --------------------------------------------------------------------------------------------- */

void
listing_cmd (void)
{
    WPanel *p;

    switch_to_listing (MENU_PANEL_IDX);

    p = PANEL (get_panel_widget (MENU_PANEL_IDX));

    p->is_panelized = FALSE;
    panel_set_filter (p, NULL);  // including panel reload
}

/* --------------------------------------------------------------------------------------------- */

void
setup_listing_format_cmd (void)
{
    int list_format;
    gboolean use_msformat;
    int brief_cols;
    char *user, *status;
    WPanel *p = NULL;

    if (SELECTED_IS_PANEL)
        p = MENU_PANEL_IDX == 0 ? left_panel : right_panel;

    list_format = panel_listing_box (p, MENU_PANEL_IDX, &user, &status, &use_msformat, &brief_cols);
    if (list_format != -1)
    {
        switch_to_listing (MENU_PANEL_IDX);
        p = MENU_PANEL_IDX == 0 ? left_panel : right_panel;
        configure_panel_listing (p, list_format, brief_cols, use_msformat, &user, &status);
        g_free (user);
        g_free (status);
    }
}

/* --------------------------------------------------------------------------------------------- */

void
panel_tree_cmd (void)
{
    create_panel (MENU_PANEL_IDX, view_tree);
}

/* --------------------------------------------------------------------------------------------- */

void
info_cmd (void)
{
    create_panel (MENU_PANEL_IDX, view_info);
}

/* --------------------------------------------------------------------------------------------- */

void
quick_view_cmd (void)
{
    if (PANEL (get_panel_widget (MENU_PANEL_IDX)) == current_panel)
        (void) change_panel ();
    create_panel (MENU_PANEL_IDX, view_quick);
}

/* --------------------------------------------------------------------------------------------- */

/* NOTE: g_spawn_command_line_sync() does not run its argument through a
 * shell, so the tools are executed with plain argv-style commands below
 * (no braces, pipes or redirections). */

char *
adb_devices_build_command (adb_devices_tool_t tool)
{
    return g_strdup (tool == ADB_DEVICES_FASTBOOT ? "fastboot devices" : "adb devices -l");
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Section header shown above each tool's devices in the panel.
 *
 * @return "[adb]" or "[fastboot]" (static string, do not free)
 */

const char *
adb_devices_section_header (adb_devices_tool_t tool)
{
    return tool == ADB_DEVICES_FASTBOOT ? "[fastboot]" : "[adb]";
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Parse one tool's output and append bare device lines to @entries.
 *
 * Each device line becomes one panel entry shown under the tool's section
 * header. Headers, blank lines and daemon chatter are skipped.
 *
 * @param output command output, may be NULL
 * @param entries array of strings to append to (takes ownership of new strings)
 */

void
adb_devices_parse_tool_output (const char *output, GPtrArray *entries)
{
    char **lines;
    int i;

    if (output == NULL)
        return;

    lines = g_strsplit (output, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        char *line = lines[i];
        char *p;
        size_t first_len;
        const char *rest;

        g_strstrip (line);
        if (*line == '\0')
            continue;
        // adb header, daemon chatter and absolute-looking paths are not devices
        if (g_str_has_prefix (line, "List of ") || line[0] == '*' || line[0] == '/')
            continue;
        // a device line is "<serial><blank><state>..."; anything else is noise
        first_len = strcspn (line, " \t");
        rest = line + first_len + strspn (line + first_len, " \t");
        if (first_len == 0 || *rest == '\0')
            continue;
        // panel entries are single-line names: collapse tabs and carriage returns
        for (p = line; *p != '\0'; p++)
            if (*p == '\t' || *p == '\r')
                *p = ' ';
        g_ptr_array_add (entries, g_strdup (line));
    }
    g_strfreev (lines);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Build a placeholder entry for a tool that reported no devices.
 *
 * Shown under the tool's section header.
 *
 * @param detail optional reason (tool error output); flattened to a single
 *               line, may be NULL or empty for plain "(no devices)"
 * @return newly allocated entry name, e.g. "(no devices)"
 */

char *
adb_devices_empty_placeholder (const char *detail)
{
    char *flat;
    char *result;
    char *p;

    if (detail == NULL || *detail == '\0')
        return g_strdup ("(no devices)");

    flat = g_strdup (detail);
    for (p = flat; *p != '\0'; p++)
        if (*p == '\n' || *p == '\t' || *p == '\r')
            *p = ' ';
    g_strstrip (flat);
    result = g_strdup_printf ("(%s)", flat);
    g_free (flat);

    return result;
}

/* --------------------------------------------------------------------------------------------- */

/* Live refresh: while a panel shows the ADB/fastboot device list, the tools
 * are re-run every second so newly attached devices appear. Implemented with
 * SIGALRM plus a self-pipe registered as a select channel, so the main loop
 * wakes up even when idle (same pattern as the SIGWINCH pipe). The timer is
 * only armed while a device panel exists; any cd in that panel clears the
 * flag (via panel_clean_dir) and the next tick disarms the timer. */
#define ADB_DEVICES_REFRESH_USEC G_USEC_PER_SEC

static int adb_devices_timer_pipe[2] = { -1, -1 };
static gboolean adb_devices_timer_channel_added = FALSE;
static gint64 adb_devices_last_refresh = 0;

/* --------------------------------------------------------------------------------------------- */

static void
adb_devices_timer_signal (int signo)
{
    char c = '\0';
    ssize_t ret;

    (void) signo;

    if (adb_devices_timer_pipe[1] != -1)
    {
        ret = write (adb_devices_timer_pipe[1], &c, 1);
        (void) ret;
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
adb_devices_timer_set (gboolean enable)
{
    struct itimerval timer;

    memset (&timer, 0, sizeof (timer));
    if (enable)
    {
        timer.it_interval.tv_sec = 1;
        timer.it_value.tv_sec = 1;
    }
    (void) setitimer (ITIMER_REAL, &timer, NULL);
}

/* --------------------------------------------------------------------------------------------- */

static void
adb_devices_timer_disarm (void)
{
    adb_devices_timer_set (FALSE);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Run both tools and collect panel entries.
 *
 * Layout per tool: a "[adb]"/"[fastboot]" header, then its devices (or a
 * single placeholder), with a blank line separating the sections.
 *
 * The timer is disarmed while spawning so children never inherit an armed
 * interval timer, and re-armed afterwards. Callers must guarantee that a
 * device panel exists (it is created right after this call, or verified
 * right before it).
 *
 * @return GPtrArray of newly allocated strings, never empty
 */

static GPtrArray *
adb_devices_collect_entries (void)
{
    static const adb_devices_tool_t tools[] = { ADB_DEVICES_ADB, ADB_DEVICES_FASTBOOT };
    GPtrArray *entries;
    guint t;

    entries = g_ptr_array_new_with_free_func (g_free);

    adb_devices_timer_disarm ();

    for (t = 0; t < G_N_ELEMENTS (tools); t++)
    {
        char *command;
        char *output = NULL;
        char *error_output = NULL;
        gint exit_status = 0;
        GError *error = NULL;
        guint before;

        g_ptr_array_add (entries, g_strdup (adb_devices_section_header (tools[t])));

        command = adb_devices_build_command (tools[t]);
        if (!g_spawn_command_line_sync (command, &output, &error_output, &exit_status, &error))
        {
            // e.g. tool binary not found on PATH: explain it in the panel
            g_ptr_array_add (entries, adb_devices_empty_placeholder (error->message));
            g_error_free (error);
        }
        else
        {
            before = entries->len;
            adb_devices_parse_tool_output (output, entries);
            if (entries->len == before)
            {
                // No devices: keep the reason (if any) visible in the panel
                const char *detail =
                    (exit_status != 0 && error_output != NULL && *error_output != '\0')
                    ? error_output
                    : NULL;

                g_ptr_array_add (entries, adb_devices_empty_placeholder (detail));
            }
        }
        g_free (command);
        g_free (output);
        g_free (error_output);

        // blank line between sections, but not after the last one
        if (t + 1 < G_N_ELEMENTS (tools))
            g_ptr_array_add (entries, g_strdup (""));
    }

    adb_devices_timer_arm ();

    // entries is never empty here: each tool contributes a header and more
    return entries;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Fill @panel with the device @entries, preserving the selected entry by name.
 *
 * The panel is switched to a single-column brief listing so the section
 * layout (headers, devices, blank separator) always renders one entry per
 * line; the previous display format is saved and restored when the device
 * list is replaced (see panel_clean_dir).
 */

static void
adb_devices_refresh_panel (WPanel *panel, GPtrArray *entries)
{
    const file_entry_t *fe;
    char *current = NULL;
    dir_list *list;
    struct stat st;
    guint i;

    fe = panel_current_entry (panel);
    if (fe != NULL)
        current = g_strndup (fe->fname->str, fe->fname->len);

    if (!panel->is_adb_devices)
    {
        panel->adb_saved_format = panel->list_format;
        panel->adb_saved_brief_cols = panel->brief_cols;
    }

    panel_clean_dir (panel);
    panel_panelize_change_root (panel, panel->cwd_vpath);
    list = &panel->dir;
    dir_list_init (list);

    // Synthetic entries: no real files behind them, read-only regular files
    memset (&st, 0, sizeof (st));
    st.st_mode = S_IFREG | 0444;

    for (i = 0; i < entries->len; i++)
    {
        if (!dir_list_append (list, entries->pdata[i], &st, FALSE, FALSE))
            break;
        file_mark (panel, list->len - 1, 0);
    }

    panel->is_panelized = TRUE;
    panel->is_adb_devices = TRUE;
    panel->list_format = list_brief;
    panel->brief_cols = 1;
    set_panel_formats (panel);
    panel_panelize_absolutize_if_needed (panel);
    panel_panelize_save (panel);

    panel_set_current_by_name (panel, current);
    g_free (current);

    // NOTE: no panel_re_sort() here: entries must stay in insertion order
    // (section headers, then devices). A plain select repaints the listing.
    select_item (panel);
}

/* --------------------------------------------------------------------------------------------- */

static int
adb_devices_timer_attention (int fd, void *data)
{
    WPanel *panels[2];
    int count = 0;
    int idx;
    char buf[64];
    GPtrArray *entries;
    int i;

    (void) data;

    // drain the pipe (non-blocking); leftovers just trigger another pass
    while (read (fd, buf, sizeof (buf)) > 0)
        ;

    // throttle to one refresh per second, no catch-up after stalls
    if (!mc_time_elapsed (&adb_devices_last_refresh, ADB_DEVICES_REFRESH_USEC))
        return 0;

    for (idx = 0; idx < 2; idx++)
    {
        WPanel *panel;

        if (get_panel_type (idx) != view_listing)
            continue;
        panel = PANEL (get_panel_widget (idx));
        if (panel->is_adb_devices)
            panels[count++] = panel;
    }

    if (count == 0)
    {
        adb_devices_timer_disarm ();
        return 0;
    }

    // refresh only over the filemanager screen, never under viewer/editor/dialogs
    if (top_dlg == NULL || top_dlg->data != (void *) filemanager)
        return 0;

    entries = adb_devices_collect_entries ();
    for (i = 0; i < count; i++)
        adb_devices_refresh_panel (panels[i], entries);
    g_ptr_array_free (entries, TRUE);

    // NOTE: no update_panels() here: it would re-stat the synthetic entries
    // and drop them. A plain repaint shows the new listing, like external
    // panelize does. Any cd in that panel restores the directory listing.
    repaint_screen ();

    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static void
adb_devices_timer_arm (void)
{
    if (!adb_devices_timer_channel_added)
    {
        struct sigaction act;

        if (pipe (adb_devices_timer_pipe) == -1)
            return;
        (void) fcntl (adb_devices_timer_pipe[0], F_SETFL, O_NONBLOCK);
        (void) fcntl (adb_devices_timer_pipe[1], F_SETFL, O_NONBLOCK);

        memset (&act, 0, sizeof (act));
        act.sa_handler = adb_devices_timer_signal;
        sigemptyset (&act.sa_mask);
#ifdef SA_RESTART
        act.sa_flags = SA_RESTART;
#endif
        my_sigaction (SIGALRM, &act, NULL);

        add_select_channel (adb_devices_timer_pipe[0], adb_devices_timer_attention, NULL);
        adb_devices_timer_channel_added = TRUE;
    }

    adb_devices_timer_set (TRUE);
}

/* --------------------------------------------------------------------------------------------- */

/**
 * Close the device list: re-read the panel's own directory. panel_clean_dir
 * (via panel_do_cd) clears the live flag and restores the display format,
 * and the refresh timer disarms itself on the next tick.
 */

static void
adb_devices_restore_panel (WPanel *panel)
{
    (void) panel_do_cd (panel, panel->cwd_vpath, cd_exact);
}

/* --------------------------------------------------------------------------------------------- */

void
adb_devices_cmd (void)
{
    WPanel *panels[2];
    int count = 0;
    int idx;
    GPtrArray *entries;
    WPanel *panel;

    // toggle: if the device list is displayed anywhere, close it
    for (idx = 0; idx < 2; idx++)
    {
        WPanel *p;

        if (get_panel_type (idx) != view_listing)
            continue;
        p = PANEL (get_panel_widget (idx));
        if (p->is_adb_devices)
            panels[count++] = p;
    }

    if (count != 0)
    {
        for (idx = 0; idx < count; idx++)
            adb_devices_restore_panel (panels[idx]);
        repaint_screen ();
        return;
    }

    entries = adb_devices_collect_entries ();

    // Show the results in the inactive panel, keep the active one as is
    if (get_other_type () != view_listing)
        create_panel (get_other_index (), view_listing);
    panel = other_panel;

    adb_devices_refresh_panel (panel, entries);
    g_ptr_array_free (entries, TRUE);

    repaint_screen ();
}

/* --------------------------------------------------------------------------------------------- */

void
encoding_cmd (void)
{
    if (SELECTED_IS_PANEL)
        panel_change_encoding (MENU_PANEL);
}

/* --------------------------------------------------------------------------------------------- */
