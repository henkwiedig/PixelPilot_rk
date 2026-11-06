#pragma once

#include "lvgl/lvgl.h"

/* colmenu integration: list the .sh files under the script dir (each strdup'd,
 * caller owns), and run one (shows the confirm dialog + streaming output). */
int  gs_scripts_collect(char **names, int max);
void gs_scripts_run(const char *name);

/* Run argv (argv[0] looked up in PATH, at most 8 arguments) behind a confirm
 * dialog showing `prompt`, in the same streaming output window titled `title`.
 * While it runs the window can only be stopped (SIGTERM, with 30 s for the
 * job's own cleanup before SIGKILL), not closed: for jobs such as an air-unit
 * update whose cleanup must run. */
void gs_scripts_run_cmd(const char *title, const char *prompt, const char *const argv[]);
