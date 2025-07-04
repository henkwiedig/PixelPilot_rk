#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <lvgl.h>
#include "executor.h"
#include "helper.h"
#include "styles.h"

#define MAX_OUTPUT_SIZE 4096
#define BUFFER_SIZE MAX_OUTPUT_SIZE * 3

typedef struct {
    char* command;
    char *stdout_output;
    char *stderr_output;
    int exit_status;
} CommandResult;


lv_group_t * current_group;
lv_group_t * error_group = NULL;
extern lv_obj_t * menu;
extern lv_indev_t * indev_drv;
lv_obj_t * msgbox = NULL;
lv_obj_t * msgbox_label = NULL;
char buffer[BUFFER_SIZE];
extern lv_group_t *loader_group;
extern lv_group_t * default_group;


void error_button_callback(lv_event_t * e) {
    lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
    menu_page_data_t* menu_page_data = lv_obj_get_user_data(current_page);
    lv_group_set_default(menu_page_data->indev_group);
    lv_indev_set_group(indev_drv,menu_page_data->indev_group);
    lv_obj_del(msgbox_label);
    lv_group_del(error_group);
    error_group = NULL;
    msgbox_label = NULL;
    msgbox = NULL;
    buffer[0] = '\0';
}


void build_output_string(char *buffer, const char *msgbox_text, CommandResult result ) {
    buffer[0] = '\0';
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "%s########\n", msgbox_text);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "command: %s\n", result.command);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "exit_status: %d\n", result.exit_status);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "stdout: %s\n", result.stdout_output);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), "stderr: %s\n", result.stderr_output);
}

void show_error(CommandResult result) {
    lv_lock();

    if (!error_group) {
        error_group = lv_group_create();
        lv_group_set_default(error_group);
        if (loader_group)
            lv_indev_set_group(indev_drv,loader_group);
        else
            lv_indev_set_group(indev_drv,error_group);
    }

    if ( ! lv_obj_is_valid(msgbox)) {
        lv_obj_t * top = lv_layer_top();
        msgbox = lv_msgbox_create(top);
        lv_obj_t * backdrop = lv_obj_get_child_by_type(top,0,&lv_msgbox_backdrop_class);
        if (backdrop)
            lv_obj_swap(backdrop, msgbox);
        lv_obj_set_style_max_height(msgbox,lv_pct(80),LV_PART_MAIN);
        lv_msgbox_add_title(msgbox, "Error");
        lv_obj_t * button = lv_msgbox_add_close_button(msgbox);
        lv_obj_add_event_cb(button, error_button_callback, LV_EVENT_DELETE,NULL);
        lv_obj_add_style(button, &style_openipc, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_style(button, &style_openipc_outline, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        msgbox_label = lv_msgbox_add_text(msgbox,"");
        // lv_label_set_long_mode(msgbox_label, LV_LABEL_LONG_MODE_SCROLL);
    };

    build_output_string(
        buffer,
        lv_label_get_text(msgbox_label),  // LVGL label text
        result
    );

    lv_label_set_text(msgbox_label,buffer);
    lv_obj_set_width(msgbox, lv_pct(80));
    lv_unlock();

}

char* run_command(const char* command) {
    CommandResult result = { NULL, NULL, NULL, -1 };

    result.command = strdup(command);

    // Create temporary files for stdout and stderr
    char stdout_file[] = "/tmp/stdout_XXXXXX";
    char stderr_file[] = "/tmp/stderr_XXXXXX";

    int stdout_fd = mkstemp(stdout_file);
    int stderr_fd = mkstemp(stderr_file);

    if (stdout_fd == -1 || stderr_fd == -1) {
        perror("Failed to create temporary files");
        free(result.command);
        return "";
    }

    // Construct the full command with redirections
    char full_command[MAX_OUTPUT_SIZE * 2];
    snprintf(full_command, sizeof(full_command),
        "%s > %s 2> %s",
        command, stdout_file, stderr_file);

    printf("Running command: %s\n", command);

    // Execute the command
    int status = system(full_command);
    result.exit_status = WEXITSTATUS(status);

    // Read stdout
    FILE *stdout_fp = fdopen(stdout_fd, "r");
    if (stdout_fp) {
        char buffer[MAX_OUTPUT_SIZE];
        result.stdout_output = malloc(MAX_OUTPUT_SIZE);
        result.stdout_output[0] = '\0';

        while (fgets(buffer, sizeof(buffer), stdout_fp)) {
            strcat(result.stdout_output, buffer);
        }
        fclose(stdout_fp);
    }

    // Read stderr
    FILE *stderr_fp = fdopen(stderr_fd, "r");
    if (stderr_fp) {
        char buffer[MAX_OUTPUT_SIZE];
        result.stderr_output = malloc(MAX_OUTPUT_SIZE);
        result.stderr_output[0] = '\0';

        while (fgets(buffer, sizeof(buffer), stderr_fp)) {
            strcat(result.stderr_output, buffer);
        }
        fclose(stderr_fp);
    }

    // Clean up temp files
    unlink(stdout_file);
    unlink(stderr_file);

    if (result.exit_status > 0) {
        show_error(result);
    }

    free(result.command);
    free(result.stderr_output);
    return result.stdout_output;
}

void* worker_thread(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;

    char * dummy = run_command(data->command);
    free(dummy);
    
    data->work_complete = true;
    return NULL;
}

void check_thread_complete(lv_timer_t* timer) {
    thread_data_t* data = (thread_data_t*)lv_timer_get_user_data(timer);    
    
    if(data->work_complete) {
        // Save callback BEFORE cleanup
        callback_fn cb = data->callback_fn;
        
        // Clean up resources
        pthread_join(data->thread_id, NULL);
        lv_obj_del(data->spinner);
        lv_timer_del(timer);
        
        // Handle error group if needed
        if (error_group) {
            lv_indev_set_group(indev_drv, error_group);
        } else {
            lv_obj_t * current_page = lv_menu_get_cur_main_page(menu);
            menu_page_data_t* menu_page_data = lv_obj_get_user_data(current_page);
            lv_indev_set_group(indev_drv,menu_page_data->indev_group);
        }

        // Free the command string if it exists
        if (data->command) {
            free(data->command);
        }
        
        // Call callback if it exists (after cleanup but before freeing data)
        if (cb != NULL) {  // Explicit NULL check
            cb();
        }
        
        // Finally free the data structure
        free(data);
    }
}

void run_command_and_block(lv_event_t* e, const char *command, callback_fn callback) {
    lv_obj_t* parent = lv_event_get_current_target(e);

    // disable input
    lv_indev_set_group(indev_drv,default_group);

    // Use calloc to zero-initialize the memory
    thread_data_t* data = calloc(1, sizeof(thread_data_t));
    if (!data) return;  // Always check allocation
    
    data->parent = parent;
    data->work_complete = false;
    data->command = strdup(command);
    data->callback_fn = callback;
    
    // Show loading screen
    data->spinner = lv_spinner_create(lv_layer_top());
    lv_obj_add_style(data->spinner,&style_openipc, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_center(data->spinner);
    
    // Create worker thread
    pthread_create(&data->thread_id, NULL, worker_thread, data);
    
    // Create timer to check for completion
    lv_timer_create(check_thread_complete, 30, data); // Check every 30ms
}

void generic_switch_event_cb(lv_event_t * e)
{
    lv_key_t key = lv_indev_get_key(indev_drv);
    if (key == LV_KEY_HOME) {
        printf("skipping change as user wants to go back");  // workaround for see: https://github.com/lvgl/lvgl/issues/8093
        return;
    }
    lv_obj_t * target = lv_event_get_target(e);
    thread_data_t * user_data = (thread_data_t *) lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");     

    if(lv_obj_has_state(target, LV_STATE_CHECKED)) {
        strcat(final_command,"on");
    } else {
        strcat(final_command,"off");
    }

    for(int i=0;i<MAX_CMD_ARGS;i++) {
        if (user_data->arguments[i]) {
            if (lv_obj_check_type(user_data->arguments[i],&lv_textarea_class)) {
                strcat(final_command," \"");
                strcat(final_command,lv_textarea_get_text(user_data->arguments[i]));
                strcat(final_command,"\"");
            }
        }
    }

    if (user_data->blocking)
        run_command(final_command);
    else
        run_command_and_block(e,final_command,NULL);
}

void generic_dropdown_event_cb(lv_event_t * e)
{
    lv_obj_t * target = lv_event_get_target(e);
    thread_data_t * user_data = (thread_data_t*) lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");
    char arg[100] = "";
    lv_dropdown_get_selected_str(target,arg,99);
    user_data->argument_string = strdup(arg);
    strcat(final_command,"\"");
    strcat(final_command,user_data->argument_string);
    strcat(final_command,"\"");

    if (user_data->blocking)
        run_command(final_command);
    else
        run_command_and_block(e,final_command,NULL);
}

void generic_slider_event_cb(lv_event_t * e)
{
    lv_obj_t * target = lv_event_get_target(e);
    lv_obj_t * slider_label = lv_obj_get_child_by_type(lv_obj_get_parent(target),1,&lv_label_class);

    int32_t *start_value = lv_obj_get_user_data(slider_label);
    if (start_value) {
        if (*start_value == lv_slider_get_value(target)) {
            return;
        }
    }
    thread_data_t * user_data = lv_event_get_user_data(e);
    char final_command[200] = "gsmenu.sh set ";
    strcat(final_command,user_data->menu_page_data->type);
    strcat(final_command," ");
    strcat(final_command,user_data->menu_page_data->page);
    strcat(final_command," ");
    strcat(final_command,user_data->parameter);
    strcat(final_command," ");
    int value;
    value = lv_slider_get_value(target);
    user_data->argument_string = malloc(32);
    sprintf(user_data->argument_string, "%i", value);
    strcat(final_command,user_data->argument_string);
    strcat(final_command," ");    
    printf("final_command: %s\n",final_command);

    if (user_data->blocking)
        run_command(final_command);
    else
        run_command_and_block(e,final_command,NULL);

    // Free previous user data if it exists
    int32_t *old_value = lv_obj_get_user_data(slider_label);
    if (old_value) free(old_value);
    lv_obj_set_user_data(slider_label,NULL);
}