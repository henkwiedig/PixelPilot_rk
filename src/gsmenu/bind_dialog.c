#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "bind_dialog.h"
#include "helper.h"
#include "styles.h"

#define BIND_COMMAND "/usr/bin/bind"

extern lv_indev_t * indev_drv;

typedef struct {
    pid_t pid;
    bool running;
    lv_timer_t *timer;
    lv_obj_t *msgbox;
    lv_obj_t *status_label;
    lv_obj_t *action_btn;
    lv_group_t *group;
    lv_group_t *return_group;
} bind_dialog_t;

static bind_dialog_t g_bind_dialog = {
    .pid = -1,
    .running = false,
};

/* Tear the dialog down: kills the child if it's still running (cancel), then
 * closes the popup and restores whatever input group owned the keypad
 * before the dialog opened. */
static void bind_dialog_close(void) {
    if (g_bind_dialog.timer) {
        lv_timer_del(g_bind_dialog.timer);
        g_bind_dialog.timer = NULL;
    }

    if (g_bind_dialog.running && g_bind_dialog.pid > 0) {
        /* setsid() in the child makes its pid a process group id too --
         * signal the whole group so nothing it spawned is left running. */
        kill(-g_bind_dialog.pid, SIGKILL);
        waitpid(g_bind_dialog.pid, NULL, 0);
    }
    g_bind_dialog.running = false;
    g_bind_dialog.pid = -1;

    if (g_bind_dialog.msgbox && lv_obj_is_valid(g_bind_dialog.msgbox)) {
        lv_msgbox_close(g_bind_dialog.msgbox);
    }
    g_bind_dialog.msgbox = NULL;
    g_bind_dialog.status_label = NULL;
    g_bind_dialog.action_btn = NULL;

    if (g_bind_dialog.group) {
        lv_group_del(g_bind_dialog.group);
        g_bind_dialog.group = NULL;
    }

    if (g_bind_dialog.return_group) {
        lv_indev_set_group(indev_drv, g_bind_dialog.return_group);
        g_bind_dialog.return_group = NULL;
    }
}

static void bind_dialog_action_cb(lv_event_t *e) {
    (void)e;
    bind_dialog_close();
}

static void bind_dialog_finish(bool success) {
    g_bind_dialog.running = false;

    if (g_bind_dialog.timer) {
        lv_timer_del(g_bind_dialog.timer);
        g_bind_dialog.timer = NULL;
    }

    if (g_bind_dialog.status_label && lv_obj_is_valid(g_bind_dialog.status_label)) {
        lv_label_set_text(g_bind_dialog.status_label,
                           success ? "Binding ... success !!!" : "Binding ... failed !!!");
    }

    if (g_bind_dialog.action_btn && lv_obj_is_valid(g_bind_dialog.action_btn)) {
        lv_obj_t *label = lv_obj_get_child(g_bind_dialog.action_btn, 0);
        if (label) {
            lv_label_set_text(label, "Close");
        }
    }
}

static void bind_dialog_poll_cb(lv_timer_t *timer) {
    (void)timer;
    if (!g_bind_dialog.running || g_bind_dialog.pid <= 0) {
        return;
    }

    int status = 0;
    pid_t ret = waitpid(g_bind_dialog.pid, &status, WNOHANG);
    if (ret != g_bind_dialog.pid) {
        return;
    }

    bind_dialog_finish(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void bind_dialog_trigger(void) {
    if (g_bind_dialog.running) {
        return; /* a bind is already in progress */
    }
    bind_dialog_close(); /* drop a stale finished dialog, if any, before reuse */

    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execl(BIND_COMMAND, BIND_COMMAND, (char *)NULL);
        _exit(127);
    }

    g_bind_dialog.pid = pid;
    g_bind_dialog.running = true;
    g_bind_dialog.return_group = lv_indev_get_group(indev_drv);

    g_bind_dialog.msgbox = lv_msgbox_create(NULL);
    lv_obj_add_style(g_bind_dialog.msgbox, &style_openipc_lightdark_background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_msgbox_add_title(g_bind_dialog.msgbox, "Bind");
    g_bind_dialog.status_label = lv_msgbox_add_text(g_bind_dialog.msgbox, "Binding ...");

    g_bind_dialog.action_btn = lv_msgbox_add_footer_button(g_bind_dialog.msgbox, "Cancel");
    lv_obj_add_style(g_bind_dialog.action_btn, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(g_bind_dialog.action_btn, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(g_bind_dialog.action_btn, bind_dialog_action_cb, LV_EVENT_CLICKED, NULL);

    theme_msgbox(g_bind_dialog.msgbox);
    lv_obj_set_width(g_bind_dialog.msgbox, 520);

    /* Dedicated input group so key navigation reaches the Cancel/Close
     * button and not whatever was focused behind the overlay. */
    g_bind_dialog.group = lv_group_create();
    lv_group_add_obj(g_bind_dialog.group, g_bind_dialog.action_btn);
    lv_indev_set_group(indev_drv, g_bind_dialog.group);

    g_bind_dialog.timer = lv_timer_create(bind_dialog_poll_cb, 100, NULL);
}
