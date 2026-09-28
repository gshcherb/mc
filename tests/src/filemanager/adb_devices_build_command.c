/*
   src/filemanager - tests for adb_devices_build_command(),
   adb_devices_section_header(), adb_devices_parse_tool_output() and
   adb_devices_empty_placeholder() functions

   Copyright (C) 2026
   Free Software Foundation, Inc.

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

#define TEST_SUITE_NAME "/src/filemanager"

#include "tests/mctest.h"

#include "src/filemanager/cmd.h"

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_build_command_per_tool)
{
    char *adb_command;
    char *fastboot_command;

    // when
    adb_command = adb_devices_build_command (ADB_DEVICES_ADB);
    fastboot_command = adb_devices_build_command (ADB_DEVICES_FASTBOOT);

    // then: plain argv-style commands, no shell syntax (g_spawn runs them directly)
    mctest_assert_not_null (adb_command);
    mctest_assert_not_null (fastboot_command);
    ck_assert_str_eq (adb_command, "adb devices -l");
    ck_assert_str_eq (fastboot_command, "fastboot devices");

    g_free (adb_command);
    g_free (fastboot_command);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_section_header)
{
    // when / then
    ck_assert_str_eq (adb_devices_section_header (ADB_DEVICES_ADB), "[adb]");
    ck_assert_str_eq (adb_devices_section_header (ADB_DEVICES_FASTBOOT), "[fastboot]");
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_parse_tool_output_adb)
{
    static const char *output =
        "List of devices attached\n"
        "emulator-5554\tdevice product:sdk_phone model:sdk_phone\n"
        "ABC123\toffline\n"
        "\n"
        "* daemon started successfully\n"
        "lonelyserial\n";

    GPtrArray *entries;

    // when
    entries = g_ptr_array_new_with_free_func (g_free);
    adb_devices_parse_tool_output (output, entries);

    // then: bare device lines, tool identity comes from the section header
    ck_assert_uint_eq (entries->len, 2);
    ck_assert_str_eq (entries->pdata[0], "emulator-5554 device product:sdk_phone model:sdk_phone");
    ck_assert_str_eq (entries->pdata[1], "ABC123 offline");

    g_ptr_array_free (entries, TRUE);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_parse_tool_output_appends)
{
    static const char *adb_output = "ABC123\tdevice\n";
    static const char *fastboot_output = "XYZ789\tfastboot\n";

    GPtrArray *entries;

    // when: both tools append to the same array, like adb_devices_cmd() does
    entries = g_ptr_array_new_with_free_func (g_free);
    adb_devices_parse_tool_output (adb_output, entries);
    adb_devices_parse_tool_output (fastboot_output, entries);

    // then
    ck_assert_uint_eq (entries->len, 2);
    ck_assert_str_eq (entries->pdata[0], "ABC123 device");
    ck_assert_str_eq (entries->pdata[1], "XYZ789 fastboot");

    g_ptr_array_free (entries, TRUE);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_parse_tool_output_noise_only)
{
    static const char *output = "List of devices attached\n\n";

    GPtrArray *entries;

    // when
    entries = g_ptr_array_new_with_free_func (g_free);
    adb_devices_parse_tool_output (output, entries);

    // then
    ck_assert_uint_eq (entries->len, 0);

    g_ptr_array_free (entries, TRUE);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_parse_tool_output_null)
{
    GPtrArray *entries;

    // when
    entries = g_ptr_array_new_with_free_func (g_free);
    adb_devices_parse_tool_output (NULL, entries);

    // then
    ck_assert_uint_eq (entries->len, 0);

    g_ptr_array_free (entries, TRUE);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_empty_placeholder_no_detail)
{
    char *null_placeholder;
    char *empty_placeholder;

    // when
    null_placeholder = adb_devices_empty_placeholder (NULL);
    empty_placeholder = adb_devices_empty_placeholder ("");

    // then
    ck_assert_str_eq (null_placeholder, "(no devices)");
    ck_assert_str_eq (empty_placeholder, "(no devices)");

    g_free (null_placeholder);
    g_free (empty_placeholder);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

START_TEST (test_adb_devices_empty_placeholder_flattens_detail)
{
    char *placeholder;

    // when: multi-line detail with tabs
    placeholder = adb_devices_empty_placeholder ("first line\n\tsecond line\n");

    // then: single panel-safe line
    ck_assert_str_eq (placeholder, "(first line  second line)");

    g_free (placeholder);
}
END_TEST

/* --------------------------------------------------------------------------------------------- */

int
main (void)
{
    TCase *tc_core;

    tc_core = tcase_create ("Core");

    // Add new tests here: ***************
    tcase_add_test (tc_core, test_adb_devices_build_command_per_tool);
    tcase_add_test (tc_core, test_adb_devices_section_header);
    tcase_add_test (tc_core, test_adb_devices_parse_tool_output_adb);
    tcase_add_test (tc_core, test_adb_devices_parse_tool_output_appends);
    tcase_add_test (tc_core, test_adb_devices_parse_tool_output_noise_only);
    tcase_add_test (tc_core, test_adb_devices_parse_tool_output_null);
    tcase_add_test (tc_core, test_adb_devices_empty_placeholder_no_detail);
    tcase_add_test (tc_core, test_adb_devices_empty_placeholder_flattens_detail);
    // ***********************************

    return mctest_run_all (tc_core);
}

/* --------------------------------------------------------------------------------------------- */
