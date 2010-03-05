/*
    Copyright (C) 2009  RedHat inc.

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
#include "report.h"
#include "run-command.h"
#include "dbus.h"
#include "abrtlib.h"
#include "DebugDump.h"
#include "CrashTypes.h" // FILENAME_* defines
#include "Plugin.h" // LoadPluginSettings
#if HAVE_CONFIG_H
# include <config.h>
#endif
#if ENABLE_NLS
# include <libintl.h>
# define _(S) gettext(S)
#else
# define _(S) (S)
#endif

/* Field separator for the crash report file that is edited by user. */
#define FIELD_SEP "%----"

/*
 * Trims whitespace characters both from left and right side of a string.
 * Modifies the string in-place. Returns the trimmed string.
 */
static char *trim(char *str)
{
  if (!str)
    return NULL;

  // Remove leading spaces.
  char *ibuf;
  ibuf = skip_whitespace(str);
  int i = strlen(ibuf);
  if (str != ibuf)
    memmove(str, ibuf, i + 1);

  // Remove trailing spaces.
  while (--i >= 0)
  {
    if (!isspace(str[i]))
      break;
  }
  str[++i] = '\0';
  return str;
}

/*
 * Escapes the field content string to avoid confusion with file comments.
 * Returned field must be free()d by caller.
 */
static char *escape(const char *str)
{
  // Determine the size of resultant string.
  // Count the required number of escape characters.
  // 1. NEWLINE followed by #
  // 2. NEWLINE followed by \# (escaped version)
  const char *ptr = str;
  bool newline = true;
  int count = 0;
  while (*ptr)
  {
    if (newline)
    {
      if (*ptr == '#')
	++count;
      if (*ptr == '\\' && ptr[1] == '#')
	++count;
    }

    newline = (*ptr == '\n');
    ++ptr;
  }

  // Copy the input string to the resultant string, and escape all
  // occurences of \# and #.
  char *result = (char*)xmalloc(strlen(str) + 1 + count);

  const char *src = str;
  char *dest = result;
  newline = true;
  while (*src)
  {
    if (newline)
    {
      if (*src == '#')
	*dest++ = '\\';
      else if (*src == '\\' && *(src + 1) == '#')
	*dest++ = '\\';
    }

    newline = (*src == '\n');
    *dest++ = *src++;
  }
  *dest = '\0';
  return result;
}

/*
 * Removes all comment lines, and unescapes the string previously escaped
 * by escape(). Works in-place.
 */
static void remove_comments_and_unescape(char *str)
{
  char *src = str, *dest = str;
  bool newline = true;
  while (*src)
  {
    if (newline)
    {
      if (*src == '#')
      { // Skip the comment line!
	while (*src && *src != '\n')
	  ++src;

	if (*src == '\0')
	  break;

	++src;
	continue;
      }
      if (*src == '\\'
       && (src[1] == '#' || (src[1] == '\\' && src[2] == '#'))
      ) {
	++src; // Unescape escaped char.
      }
    }

    newline = (*src == '\n');
    *dest++ = *src++;
  }
  *dest = '\0';
}

/*
 * Writes a field of crash report to a file.
 * Field must be writable.
 */
static void write_crash_report_field(FILE *fp, const map_crash_data_t &report,
				     const char *field, const char *description)
{
  const map_crash_data_t::const_iterator it = report.find(field);
  if (it == report.end())
  {
    // exit silently, all fields are optional for now
    //error_msg("Field %s not found.\n", field);
    return;
  }

  if (it->second[CD_TYPE] == CD_SYS)
  {
    error_msg("Cannot write field %s because it is a system value\n", field);
    return;
  }

  fprintf(fp, "%s%s\n", FIELD_SEP, it->first.c_str());

  fprintf(fp, "%s\n", description);
  if (it->second[CD_EDITABLE] != CD_ISEDITABLE)
    fprintf(fp, _("# This field is read only.\n"));

  char *escaped_content = escape(it->second[CD_CONTENT].c_str());
  fprintf(fp, "%s\n", escaped_content);
  free(escaped_content);
}

/*
 * Saves the crash report to a file.
 * Parameter 'fp' must be opened before write_crash_report is called.
 * Returned value:
 *  If the report is successfully stored to the file, a zero value is returned.
 *  On failure, nonzero value is returned.
 */
static void write_crash_report(const map_crash_data_t &report, FILE *fp)
{
  fprintf(fp, "# Please check this report. Lines starting with '#' will be ignored.\n"
	  "# Lines starting with '%%----' separate fields, please do not delete them.\n\n");

  write_crash_report_field(fp, report, FILENAME_COMMENT,
			   _("# Describe the circumstances of this crash below."));
  write_crash_report_field(fp, report, FILENAME_REPRODUCE,
			   _("# How to reproduce the crash?"));
  write_crash_report_field(fp, report, FILENAME_BACKTRACE,
			   _("# Backtrace\n# Check that it does not contain any sensitive data such as passwords."));
  write_crash_report_field(fp, report, CD_DUPHASH, "# DUPHASH");
  write_crash_report_field(fp, report, FILENAME_ARCHITECTURE, _("# Architecture"));
  write_crash_report_field(fp, report, FILENAME_CMDLINE, _("# Command line"));
  write_crash_report_field(fp, report, FILENAME_COMPONENT, _("# Component"));
  write_crash_report_field(fp, report, FILENAME_COREDUMP, _("# Core dump"));
  write_crash_report_field(fp, report, FILENAME_EXECUTABLE, _("# Executable"));
  write_crash_report_field(fp, report, FILENAME_KERNEL, _("# Kernel version"));
  write_crash_report_field(fp, report, FILENAME_PACKAGE, _("# Package"));
  write_crash_report_field(fp, report, FILENAME_REASON, _("# Reason of crash"));
  write_crash_report_field(fp, report, FILENAME_RELEASE, _("# Release string of the operating system"));
}

/*
 * Updates appropriate field in the report from the text. The text can
 * contain multiple fields.
 * Returns:
 *  0 if no change to the field was detected.
 *  1 if the field was changed.
 *  Changes to read-only fields are ignored.
 */
static int read_crash_report_field(const char *text, map_crash_data_t &report,
				   const char *field)
{
  char separator[sizeof("\n" FIELD_SEP)-1 + strlen(field) + 2]; // 2 = '\n\0'
  sprintf(separator, "\n%s%s\n", FIELD_SEP, field);
  const char *textfield = strstr(text, separator);
  if (!textfield)
    return 0; // exit silently because all fields are optional

  textfield += strlen(separator);
  int length = 0;
  const char *end = strstr(textfield, "\n" FIELD_SEP);
  if (!end)
    length = strlen(textfield);
  else
    length = end - textfield;

  const map_crash_data_t::iterator it = report.find(field);
  if (it == report.end())
  {
    error_msg("Field %s not found.\n", field);
    return 0;
  }

  if (it->second[CD_TYPE] == CD_SYS)
  {
    error_msg("Cannot update field %s because it is a system value.\n", field);
    return 0;
  }

  // Do not change noneditable fields.
  if (it->second[CD_EDITABLE] != CD_ISEDITABLE)
    return 0;

  // Compare the old field contents with the new field contents.
  char newvalue[length + 1];
  strncpy(newvalue, textfield, length);
  newvalue[length] = '\0';
  trim(newvalue);

  char oldvalue[it->second[CD_CONTENT].length() + 1];
  strcpy(oldvalue, it->second[CD_CONTENT].c_str());
  trim(oldvalue);

  // Return if no change in the contents detected.
  int cmp = strcmp(newvalue, oldvalue);
  if (!cmp)
    return 0;

  it->second[CD_CONTENT].assign(newvalue);
  return 1;
}

/*
 * Updates the crash report 'report' from the text. The text must not contain
 * any comments.
 * Returns:
 *  0 if no field was changed.
 *  1 if any field was changed.
 *  Changes to read-only fields are ignored.
 */
static int read_crash_report(map_crash_data_t &report, const char *text)
{
  int result = 0;
  result |= read_crash_report_field(text, report, FILENAME_COMMENT);
  result |= read_crash_report_field(text, report, FILENAME_REPRODUCE);
  result |= read_crash_report_field(text, report, FILENAME_BACKTRACE);
  result |= read_crash_report_field(text, report, CD_DUPHASH);
  result |= read_crash_report_field(text, report, FILENAME_ARCHITECTURE);
  result |= read_crash_report_field(text, report, FILENAME_CMDLINE);
  result |= read_crash_report_field(text, report, FILENAME_COMPONENT);
  result |= read_crash_report_field(text, report, FILENAME_COREDUMP);
  result |= read_crash_report_field(text, report, FILENAME_EXECUTABLE);
  result |= read_crash_report_field(text, report, FILENAME_KERNEL);
  result |= read_crash_report_field(text, report, FILENAME_PACKAGE);
  result |= read_crash_report_field(text, report, FILENAME_REASON);
  result |= read_crash_report_field(text, report, FILENAME_RELEASE);
  return result;
}

/**
 * Runs external editor.
 * Returns:
 *  0 if the launch was successful
 *  1 if it failed. The error reason is logged using error_msg()
 */
static int launch_editor(const char *path)
{
  const char *editor, *terminal;

  editor = getenv("ABRT_EDITOR");
  if (!editor)
    editor = getenv("VISUAL");
  if (!editor)
    editor = getenv("EDITOR");

  terminal = getenv("TERM");
  if (!editor && (!terminal || !strcmp(terminal, "dumb")))
  {
    error_msg(_("Terminal is dumb but no VISUAL nor EDITOR defined."));
    return 1;
  }

  if (!editor)
    editor = "vi";

  char *args[3];
  args[0] = (char*)editor;
  args[1] = (char*)path;
  args[2] = NULL;
  run_command(args);

  return 0;
}

/**
 * Returns:
 *  0 on success, crash data has been updated
 *  2 on failure, unable to create, open, or close temporary file
 *  3 on failure, cannot launch text editor
 */
static int run_report_editor(map_crash_data_t &cr)
{
  /* Open a temporary file and write the crash report to it. */
  char filename[] = "/tmp/abrt-report.XXXXXX";
  int fd = mkstemp(filename);
  if (fd == -1) /* errno is set */
  {
    perror_msg("can't generate temporary file name");
    return 2;
  }

  FILE *fp = fdopen(fd, "w");
  if (!fp) /* errno is set */
  {
    perror_msg("can't open '%s' to save the crash report", filename);
    return 2;
  }

  write_crash_report(cr, fp);

  if (fclose(fp)) /* errno is set */
  {
    perror_msg("can't close '%s'", filename);
    return 2;
  }

  // Start a text editor on the temporary file.
  if (launch_editor(filename) != 0)
    return 3; /* exit with error */

  // Read the file back and update the report from the file.
  fp = fopen(filename, "r");
  if (!fp) /* errno is set */
  {
    perror_msg("can't open '%s' to read the crash report", filename);
    return 2;
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  char *text = (char*)xmalloc(size + 1);
  if (fread(text, 1, size, fp) != size)
  {
    error_msg("can't read '%s'", filename);
    return 2;
  }
  text[size] = '\0';
  if (fclose(fp) != 0) /* errno is set */
  {
    perror_msg("can't close '%s'", filename);
    return 2;
  }

  // Delete the tempfile.
  if (unlink(filename) == -1) /* errno is set */
  {
    perror_msg("can't unlink %s", filename);
  }

  remove_comments_and_unescape(text);
  // Updates the crash report from the file text.
  int report_changed = read_crash_report(cr, text);
  free(text);
  if (report_changed)
    puts(_("\nThe report has been updated."));
  else
    puts(_("\nNo changes were detected in the report."));

  return 0;
}

/**
 * Asks user for a text response.
 * @param question
 *  Question displayed to user.
 * @param result
 *  Output array.
 * @param result_size
 *  Maximum byte count to be written.
 */
static void read_from_stdin(const char *question, char *result, int result_size)
{
  printf("%s", question);
  fflush(NULL);
  fgets(result, result_size, stdin);
  // Remove the newline from the login.
  char *newline = strchr(result, '\n');
  if (newline)
    *newline = '\0';
}

/**
 * Gets reporter plugin settings.
 * @param ask_user
 *   If it's set to true and some reporter plugin settings are found to be missing
 *   (like login name or password), user is asked to provide the missing parts.
 * @param settings
 *   A structure filled with reporter plugin settings.
 */
static void get_reporter_plugin_settings(map_map_string_t &settings, bool ask_user)
{
  /* First of all, load system-wide report plugin settings. */
  // Get informations about all plugins.
  map_map_string_t plugins = call_GetPluginsInfo();
  // Check the configuration of each enabled Reporter plugin.
  map_map_string_t::iterator it, itend = plugins.end();
  for (it = plugins.begin(); it != itend; ++it)
  {
    // Skip disabled plugins.
    if (0 != strcmp(it->second["Enabled"].c_str(), "yes"))
      continue;
    // Skip nonReporter plugins.
    if (0 != strcmp(it->second["Type"].c_str(), "Reporter"))
      continue;
    map_string_t single_plugin_settings = call_GetPluginSettings(it->first.c_str());
    // Copy the received settings as defaults.
    // Plugins won't work without it, if some value is missing
    // they use their default values for all fields.
    settings[it->first] = single_plugin_settings;
  }

  /* Second, load user-specific settings, which override
     the system-wide settings. */
  struct passwd* pw = getpwuid(geteuid());
  const char* homedir = pw ? pw->pw_dir : NULL;
  if (homedir)
  {
    itend = settings.end();
    for (it = settings.begin(); it != itend; ++it)
    {
      map_string_t single_plugin_settings;
      std::string path = std::string(homedir) + "/.abrt/"
	+ it->first + "."PLUGINS_CONF_EXTENSION;
      /* Load plugin config in the home dir. Do not skip lines with empty value (but containing a "key="),
         because user may want to override password from /etc/abrt/plugins/*.conf, but he prefers to
         enter it every time he reports. */
      bool success = LoadPluginSettings(path.c_str(), single_plugin_settings, false);
      if (!success)
	continue;
      // Merge user's plugin settings into already loaded settings.
      map_string_t::const_iterator valit, valitend = single_plugin_settings.end();
      for (valit = single_plugin_settings.begin(); valit != valitend; ++valit)
	it->second[valit->first] = valit->second;
    }
  }

  if (!ask_user)
    return;

  /* Third, check if a login or password is missing, and ask for it. */
  itend = settings.end();
  for (it = settings.begin(); it != itend; ++it)
  {
    map_string_t &single_plugin_settings = it->second;
    // Login information is missing.
    bool loginMissing = single_plugin_settings.find("Login") != single_plugin_settings.end()
      && 0 == strcmp(single_plugin_settings["Login"].c_str(), "");
    bool passwordMissing = single_plugin_settings.find("Password") != single_plugin_settings.end()
      && 0 == strcmp(single_plugin_settings["Password"].c_str(), "");
    if (!loginMissing && !passwordMissing)
      continue;

    // Read the missing information and push it to plugin settings.
    printf(_("Wrong settings were detected for plugin %s.\n"), it->first.c_str());
    char result[64];
    if (loginMissing)
    {
      read_from_stdin(_("Enter your login: "), result, 64);
      single_plugin_settings["Login"] = std::string(result);
    }
    if (passwordMissing)
    {
// TODO: echo off, see http://fixunix.com/unix/84474-echo-off.html
      read_from_stdin(_("Enter your password: "), result, 64);
      single_plugin_settings["Password"] = std::string(result);
    }
  }
}

/* Reports the crash with corresponding crash_id over DBus. */
int report(const char *crash_id, bool always)
{
  // Ask for an initial report.
  map_crash_data_t cr = call_CreateReport(crash_id);
  if (cr.size() == 0)
  {
    return -1;
  }

  /* Open text editor and give a chance to review the backtrace etc. */
  if (!always)
  {
    int result = run_report_editor(cr);
    if (result != 0)
      return result;
  }

  /* Read the plugin settings. */
  map_map_string_t pluginSettings;
  get_reporter_plugin_settings(pluginSettings, !always);

  /* Ask if user really wants to send the report. */
  if (!always)
  {
    // Report only if the user is sure.
    printf(_("Do you want to send the report? [y/N]: "));
    fflush(NULL);
    char answer[16] = "n";
    fgets(answer, sizeof(answer), stdin);
    if ((answer[0] | 0x20) != 'y')
    {
      puts(_("Crash report was not sent."));
      return 0;
    }
  }

  int errors = 0;
  int plugins = 0;
  puts(_("Reporting..."));
  report_status_t r = call_Report(cr, pluginSettings);
  report_status_t::iterator it = r.begin();
  while (it != r.end())
  {
    vector_string_t &v = it->second;
    printf("%s: %s\n", it->first.c_str(), v[REPORT_STATUS_IDX_MSG].c_str());
    plugins++;
    if (v[REPORT_STATUS_IDX_FLAG] == "0")
      errors++;
    it++;
  }
  printf(_("Crash reported via %d plugins (%d errors)\n"), plugins, errors);

  return errors != 0;
}
