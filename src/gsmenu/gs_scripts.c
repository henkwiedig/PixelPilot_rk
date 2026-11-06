#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "../input.h"
#include "colmenu.h"
#include "helper.h"
#include "gs_scripts.h"
#include "styles.h"

#define SCRIPT_DIR "/media/dvr/scripts"
#define MAX_SCRIPT_COUNT 128
#define MAX_OUTPUT_KEEP 32768
#define OUTPUT_CHUNK 512

extern lv_indev_t * indev_drv;
extern lv_group_t * default_group;

typedef struct {
    pid_t pid;
    int fd;
    bool running;
    lv_timer_t *timer;
    lv_obj_t *msgbox;
    lv_obj_t *output_ta;
    lv_obj_t *close_btn;
    lv_obj_t *stop_btn;
    char *output;
    size_t output_len;
    lv_group_t *group;
    uint32_t term_tick;   /* tick when SIGTERM was sent (0 = not stopping); used
                             to escalate to SIGKILL if the script ignores it   */
    bool locked;          /* Close is disabled while the child runs: a job whose
                             own cleanup must run can only be stopped (SIGTERM) */
    int esc;              /* ANSI escape parser state, kept across reads        */
    bool cr;              /* a "\r" came last: the next text rewrites the line  */
} script_runner_t;

enum { ESC_NONE, ESC_START, ESC_CSI };

#define MAX_CMD_ARGS 8

static char *g_script_names[MAX_SCRIPT_COUNT];
static int g_script_count;
static lv_obj_t *g_confirm_msgbox;
static lv_group_t *g_confirm_group;
static lv_timer_t *g_confirm_timer;   /* keeps the keypad on the dialog */
static char g_pending_script[256];
/* gs_scripts_run_cmd(): the command waiting for its confirm dialog (argv
 * strdup'd, NULL-terminated; g_pending_argv[0] == NULL = a script is pending) */
static char *g_pending_argv[MAX_CMD_ARGS + 1];
static char g_pending_title[128];
static char g_pending_prompt[640];

static script_runner_t g_runner = {
    .pid = -1,
    .fd = -1,
    .running = false,
    .timer = NULL,
    .msgbox = NULL,
    .output_ta = NULL,
    .close_btn = NULL,
    .stop_btn = NULL,
    .output = NULL,
    .output_len = 0,
    .group = NULL,
};

static int cmp_string_ptr(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static bool ends_with_sh(const char *name)
{
    size_t len = strlen(name);
    return len > 3 && strcmp(name + len - 3, ".sh") == 0;
}

static int collect_scripts(char **scripts, int max_scripts)
{
    DIR *dir = opendir(SCRIPT_DIR);
    if (!dir) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_scripts) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!ends_with_sh(entry->d_name)) {
            continue;
        }

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", SCRIPT_DIR, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        scripts[count] = strdup(entry->d_name);
        if (scripts[count]) {
            count++;
        }
    }

    closedir(dir);
    qsort(scripts, count, sizeof(char *), cmp_string_ptr);
    return count;
}

int gs_scripts_collect(char **names, int max)
{
    return collect_scripts(names, max);
}

static void clear_script_names(void)
{
    for (int i = 0; i < g_script_count; i++) {
        free(g_script_names[i]);
        g_script_names[i] = NULL;
    }
    g_script_count = 0;
}

/* Appends child output the way a terminal shows it: a "\r" that is not part of
 * "\r\n" returns to the start of the line, so the text after it rewrites that
 * line (progress counters, wget's bar stay on one line), and ANSI escape
 * sequences (colours) and other control bytes are dropped. The escape and "\r"
 * state carry over from one read to the next. */
static void runner_append(const char *text)
{
    if (!text) {
        return;
    }

    size_t add = strlen(text);
    if (add == 0) {
        return;
    }

    char *new_buf = realloc(g_runner.output, g_runner.output_len + add + 1);
    if (!new_buf) {
        return;
    }
    g_runner.output = new_buf;

    char *out = g_runner.output;
    size_t len = g_runner.output_len;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;
        if (g_runner.esc == ESC_CSI) {          /* parameters until the final byte */
            if (c >= 0x40 && c <= 0x7e) {
                g_runner.esc = ESC_NONE;
            }
            continue;
        }
        if (g_runner.esc == ESC_START) {        /* ESC [ ... or a two-byte escape */
            g_runner.esc = (c == '[') ? ESC_CSI : ESC_NONE;
            continue;
        }
        if (c == 0x1b) {
            g_runner.esc = ESC_START;
            continue;
        }
        if (c == '\r') {
            g_runner.cr = true;
            continue;
        }
        if (c == '\n') {
            g_runner.cr = false;
            out[len++] = '\n';
            continue;
        }
        if (c < 0x20 && c != '\t') {
            continue;
        }
        if (g_runner.cr) {
            while (len > 0 && out[len - 1] != '\n') {
                len--;
            }
            g_runner.cr = false;
        }
        out[len++] = (char)c;
    }
    out[len] = '\0';

    if (len > MAX_OUTPUT_KEEP) {
        size_t drop = len - MAX_OUTPUT_KEEP;
        memmove(out, out + drop, len - drop + 1);
        len -= drop;
    }
    g_runner.output_len = len;

    if (g_runner.output_ta && lv_obj_is_valid(g_runner.output_ta)) {
        lv_obj_t *ta = g_runner.output_ta;
        lv_textarea_set_text(ta, g_runner.output);
        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
        /* scroll the newest output into view */
        lv_obj_update_layout(ta);
        int32_t sb = lv_obj_get_scroll_bottom(ta);
        if (sb > 0) {
            lv_obj_scroll_by(ta, 0, -sb, LV_ANIM_OFF);
        }
    }
}

static void runner_finish_message(int status)
{
    char line[96];
    if (WIFEXITED(status)) {
        snprintf(line, sizeof(line), "\n\nProcess exited with code %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(line, sizeof(line), "\n\nProcess terminated by signal %d\n", WTERMSIG(status));
    } else {
        snprintf(line, sizeof(line), "\n\nProcess ended\n");
    }
    runner_append(line);
}

static void script_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* The menu behind may take the keypad meanwhile (closing the drone
     * columns when the link drops, as during an air unit's reboot): keep it on
     * Stop/Close while the window is open. */
    if (g_runner.group && lv_indev_get_group(indev_drv) != g_runner.group) {
        lv_indev_set_group(indev_drv, g_runner.group);
    }

    if (g_runner.fd >= 0) {
        char chunk[OUTPUT_CHUNK + 1];
        ssize_t r;
        while ((r = read(g_runner.fd, chunk, OUTPUT_CHUNK)) > 0) {
            chunk[r] = '\0';
            runner_append(chunk);
        }
    }

    if (g_runner.running && g_runner.pid > 0) {
        /* Escalate to SIGKILL if the script ignored SIGTERM for over a second;
         * a locked job's cleanup (e.g. ssh to the air unit) gets 30 s. */
        if (g_runner.term_tick &&
            lv_tick_elaps(g_runner.term_tick) > (g_runner.locked ? 30000u : 1000u)) {
            kill(-g_runner.pid, SIGKILL);
            g_runner.term_tick = 0;
        }
        int status = 0;
        pid_t ret = waitpid(g_runner.pid, &status, WNOHANG);
        if (ret == g_runner.pid) {
            g_runner.running = false;
            g_runner.term_tick = 0;
            runner_finish_message(status);
            if (g_runner.fd >= 0) {
                close(g_runner.fd);
                g_runner.fd = -1;
            }
            if (g_runner.stop_btn && lv_obj_is_valid(g_runner.stop_btn)) {
                lv_obj_add_state(g_runner.stop_btn, LV_STATE_DISABLED);
            }
            if (g_runner.locked && g_runner.close_btn && lv_obj_is_valid(g_runner.close_btn)) {
                lv_obj_remove_state(g_runner.close_btn, LV_STATE_DISABLED);
                if (g_runner.group) {
                    lv_group_focus_obj(g_runner.close_btn);
                }
            }
        }
    }
}

static void close_runner_ui(void)
{
    if (g_runner.timer) {
        lv_timer_del(g_runner.timer);
        g_runner.timer = NULL;
    }

    if (g_runner.fd >= 0) {
        close(g_runner.fd);
        g_runner.fd = -1;
    }

    if (g_runner.running && g_runner.pid > 0) {
        /* Tearing down — force-kill the whole process group (SIGKILL can't be
         * ignored) so nothing is left running when the dialog closes. */
        kill(-g_runner.pid, SIGKILL);
        waitpid(g_runner.pid, NULL, 0);
    }

    g_runner.running = false;
    g_runner.pid = -1;
    g_runner.term_tick = 0;
    g_runner.locked = false;
    g_runner.esc = ESC_NONE;
    g_runner.cr = false;

    if (g_runner.msgbox && lv_obj_is_valid(g_runner.msgbox)) {
        lv_msgbox_close(g_runner.msgbox);
    }
    g_runner.msgbox = NULL;
    g_runner.output_ta = NULL;
    g_runner.close_btn = NULL;
    g_runner.stop_btn = NULL;

    free(g_runner.output);
    g_runner.output = NULL;
    g_runner.output_len = 0;

    if (g_runner.group) {
        lv_group_del(g_runner.group);
        g_runner.group = NULL;
    }

    /* Back to the menu: its current column (the one the run was launched
     * from may have closed meanwhile, e.g. when the drone link dropped). */
    colmenu_refocus();
}

static void script_close_cb(lv_event_t *e)
{
    (void)e;
    if (g_runner.locked && g_runner.running) {
        return;   /* only Stop while a locked job runs */
    }
    close_runner_ui();
}

static void script_stop_cb(lv_event_t *e)
{
    (void)e;
    if (g_runner.running && g_runner.pid > 0) {
        /* The child did setsid(), so it leads its own process group. Signal the
         * whole GROUP (negative pid) — otherwise a foreground child of the shell
         * (a loop, a sleep, a spawned tool) keeps running and the script never
         * actually stops. */
        kill(-g_runner.pid, SIGTERM);
        g_runner.term_tick = lv_tick_get() ? lv_tick_get() : 1;
        runner_append(g_runner.locked ? "\nStopping...\n" : "\nStopping script...\n");
    }
}

/* Runs `file` (looked up in PATH) with argv on a pty in its own session, in
 * cwd, and shows its output in a window titled `title` that starts with
 * `header`. A locked run can only be stopped, not closed, until it exits. */
static void start_runner(const char *title, const char *cwd, const char *file,
                         char *const argv[], bool locked, const char *header)
{
    if (g_runner.locked && g_runner.running) {
        return;   /* never kill a locked job by starting another */
    }

    close_runner_ui();

    /* Use a pseudoterminal so the child sees a tty on stdout/stderr.
     * This causes shells and most programs to flush output line by line,
     * giving continuous log output instead of large buffered chunks.
     * Use the Linux-native /dev/ptmx approach to avoid needing posix_openpt()
     * which may not be available on musl-based cross-compilers. */
    int master_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        return;
    }
    /* Unlock the slave pty (equivalent to unlockpt()). */
    {
        int unlock = 0;
        ioctl(master_fd, TIOCSPTLCK, &unlock);
    }
    /* Get the slave pty device number (equivalent to ptsname()). */
    unsigned int ptnum = 0;
    if (ioctl(master_fd, TIOCGPTN, &ptnum) != 0) {
        close(master_fd);
        return;
    }
    char slave_path[64];
    snprintf(slave_path, sizeof(slave_path), "/dev/pts/%u", ptnum);

    /* Disable echo and LF->CRLF translation so output reads as plain text. */
    {
        int slave_fd_temp = open(slave_path, O_RDWR | O_NOCTTY);
        if (slave_fd_temp >= 0) {
            struct termios tios;
            if (tcgetattr(slave_fd_temp, &tios) == 0) {
                tios.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
                tios.c_oflag &= ~ONLCR;
                tcsetattr(slave_fd_temp, TCSANOW, &tios);
            }
            close(slave_fd_temp);
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        return;
    }

    if (pid == 0) {
        /* Become a new session leader so the pty becomes the controlling
         * terminal, which forces line-buffered output from the child. */
        setsid();
        int slave_fd = open(slave_path, O_RDWR);
        if (slave_fd < 0) {
            _exit(127);
        }
        ioctl(slave_fd, TIOCSCTTY, 0);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);
        close(master_fd);

        if (cwd && chdir(cwd) != 0) {
            perror("chdir");
            _exit(127);
        }

        execvp(file, argv);
        perror("execvp");
        _exit(127);
    }

    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }

    g_runner.pid = pid;
    g_runner.fd = master_fd;
    g_runner.running = true;
    g_runner.locked = locked;

    g_runner.msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(g_runner.msgbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_msgbox_add_title(g_runner.msgbox, title);

    lv_obj_t *content = lv_msgbox_get_content(g_runner.msgbox);
    lv_obj_add_style(content, &style_openipc_dark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(content, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_runner.output_ta = lv_textarea_create(content);
    /* The textarea must be a child of the msgbox content container, otherwise
     * LVGL lays it out beside the built-in header/content/footer regions and
     * most of the modal ends up unused. */
    lv_obj_set_size(g_runner.output_ta, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(g_runner.output_ta, 1);
    lv_obj_add_style(g_runner.output_ta, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.output_ta, &style_openipc_textcolor, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.output_ta, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_textarea_set_one_line(g_runner.output_ta, false);
    lv_textarea_set_password_mode(g_runner.output_ta, false);
    lv_textarea_set_cursor_click_pos(g_runner.output_ta, false);
    lv_obj_set_scrollbar_mode(g_runner.output_ta, LV_SCROLLBAR_MODE_ACTIVE);

    g_runner.stop_btn = lv_msgbox_add_footer_button(g_runner.msgbox, "Stop");
    g_runner.close_btn = lv_msgbox_add_footer_button(g_runner.msgbox, "Close");
    lv_obj_add_event_cb(g_runner.stop_btn, script_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(g_runner.close_btn, script_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_style(g_runner.stop_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.stop_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(g_runner.close_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_runner.close_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    if (locked) {
        lv_obj_add_state(g_runner.close_btn, LV_STATE_DISABLED);
    }

    theme_msgbox(g_runner.msgbox);

    /* Fixed size: triggers msgbox_size_changed_event_cb which sets flex_grow=1
     * on the content area, making the textarea fill the available height. */
    lv_obj_set_size(g_runner.msgbox, lv_pct(98), lv_pct(92));

    /* Create a dedicated input group for this dialog so key navigation
     * reaches Stop/Close and not the script list behind the overlay. */
    g_runner.group = lv_group_create();
    lv_group_add_obj(g_runner.group, g_runner.stop_btn);
    lv_group_add_obj(g_runner.group, g_runner.close_btn);
    lv_indev_set_group(indev_drv, g_runner.group);

    runner_append(header);

    g_runner.timer = lv_timer_create(script_poll_timer_cb, 10, NULL);
}

static void start_script_runner(const char *script_name)
{
    if (!script_name || strchr(script_name, '/')) {
        return;
    }

    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", SCRIPT_DIR, script_name);

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
        mode_t new_mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
        if (new_mode != st.st_mode) {
            if (chmod(fullpath, new_mode) != 0) {
                perror("chmod");
            }
        }
    }

    char header[512];
    snprintf(header, sizeof(header), "Running %s/%s\n\n", SCRIPT_DIR, script_name);
    char *const argv[] = { "sh", (char *)script_name, NULL };
    start_runner(script_name, SCRIPT_DIR, "/bin/sh", argv, false, header);
}

static void clear_pending_cmd(void)
{
    for (int i = 0; i < MAX_CMD_ARGS && g_pending_argv[i]; i++) {
        free(g_pending_argv[i]);
        g_pending_argv[i] = NULL;
    }
}

/* Starts the pending command: the window's first lines show what runs. */
static void start_pending_cmd(void)
{
    char header[512];
    size_t n = snprintf(header, sizeof(header), "Running");
    for (int i = 0; g_pending_argv[i] && n < sizeof(header); i++) {
        n += snprintf(header + n, sizeof(header) - n, " %s", g_pending_argv[i]);
    }
    if (n < sizeof(header)) {
        snprintf(header + n, sizeof(header) - n, "\n\n");
    }
    start_runner(g_pending_title, "/", g_pending_argv[0], g_pending_argv, true, header);
    clear_pending_cmd();
}

/* As in script_poll_timer_cb: the menu behind must not take the keypad. */
static void confirm_guard_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_confirm_group && lv_indev_get_group(indev_drv) != g_confirm_group) {
        lv_indev_set_group(indev_drv, g_confirm_group);
    }
}

static void close_confirm(void)
{
    if (g_confirm_timer) {
        lv_timer_delete(g_confirm_timer);
        g_confirm_timer = NULL;
    }
    if (g_confirm_msgbox && lv_obj_is_valid(g_confirm_msgbox)) {
        lv_msgbox_close(g_confirm_msgbox);
    }
    g_confirm_msgbox = NULL;
    if (g_confirm_group) {
        lv_group_del(g_confirm_group);
        g_confirm_group = NULL;
    }
}

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    close_confirm();
    clear_pending_cmd();
    colmenu_refocus();
}

static void confirm_execute_cb(lv_event_t *e)
{
    (void)e;
    close_confirm();
    if (g_pending_argv[0]) {
        start_pending_cmd();
    } else {
        start_script_runner(g_pending_script);
    }
}

/* Build the confirm dialog for g_pending_script, or the pending command when
 * there is one (its own title and prompt). Deferred via lv_async_call from
 * gs_scripts_run so the key press that launched it has released first (colmenu
 * actions fire on the key press, and the release would otherwise click a button). */
static void show_confirm_dialog(void *unused)
{
    (void)unused;
    close_confirm();

    g_confirm_msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(g_confirm_msgbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    bool cmd = g_pending_argv[0] != NULL;
    lv_msgbox_add_title(g_confirm_msgbox, cmd ? g_pending_title : "Confirm Execution");

    char prompt[512];
    if (cmd) {
        lv_msgbox_add_text(g_confirm_msgbox, g_pending_prompt);
    } else {
        snprintf(prompt, sizeof(prompt),
                 "Are you sure you want to execute\n%s?\n\n"
                 "Check the source of this script first. "
                 "It can modify and brick your system.",
                 g_pending_script);
        lv_msgbox_add_text(g_confirm_msgbox, prompt);
    }

    lv_obj_t *cancel_btn = lv_msgbox_add_footer_button(g_confirm_msgbox, "Cancel");
    lv_obj_t *exec_btn = lv_msgbox_add_footer_button(g_confirm_msgbox, cmd ? "Start" : "Execute");

    lv_obj_add_style(cancel_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(cancel_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_style(exec_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(exec_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);

    lv_obj_add_event_cb(cancel_btn, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(exec_btn, confirm_execute_cb, LV_EVENT_CLICKED, NULL);

    theme_msgbox(g_confirm_msgbox);

    lv_obj_set_width(g_confirm_msgbox, lv_pct(75));

    /* Create a dedicated input group so key navigation reaches Cancel/Execute
     * and not the script list behind the overlay. */
    g_confirm_group = lv_group_create();
    lv_group_add_obj(g_confirm_group, cancel_btn);
    lv_group_add_obj(g_confirm_group, exec_btn);
    lv_group_focus_obj(cancel_btn);   /* default to the safe choice */
    lv_indev_set_group(indev_drv, g_confirm_group);
    g_confirm_timer = lv_timer_create(confirm_guard_cb, 50, NULL);
    /* the key press that opened this (colmenu actions fire on press) is still
     * down — without this its release would immediately click a button. */
    lv_indev_wait_release(indev_drv);
}

void gs_scripts_run(const char *name)
{
    if (!name || strchr(name, '/')) {
        return;
    }
    clear_pending_cmd();
    snprintf(g_pending_script, sizeof(g_pending_script), "%s", name);
    show_confirm_dialog(NULL);
}

void gs_scripts_run_cmd(const char *title, const char *prompt, const char *const argv[])
{
    if (!title || !prompt || !argv || !argv[0]) {
        return;
    }
    if (g_runner.locked && g_runner.running) {
        return;
    }
    clear_pending_cmd();
    for (int i = 0; i < MAX_CMD_ARGS && argv[i]; i++) {
        g_pending_argv[i] = strdup(argv[i]);
        if (!g_pending_argv[i]) {
            clear_pending_cmd();
            return;
        }
    }
    snprintf(g_pending_title, sizeof(g_pending_title), "%s", title);
    snprintf(g_pending_prompt, sizeof(g_pending_prompt), "%s", prompt);
    show_confirm_dialog(NULL);
}

