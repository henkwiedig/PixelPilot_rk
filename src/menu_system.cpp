#include "menu_system.hpp"
#include "drm.h"
#include "spdlog.h"

#define NK_IMPLEMENTATION
#define NK_INCLUDE_STANDARD_IO
// #define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
// #define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
// #define NK_INCLUDE_FONT_BAKING
// #define NK_INCLUDE_DEFAULT_FONT
// #define NK_INCLUDE_COMMAND_USERDATA
#include "nuklear_console/vendor/Nuklear/nuklear.h"
// Gamepad support https://github.com/robloach/nuklear_gamepad
#define NK_GAMEPAD_IMPLEMENTATION
#define NK_GAMEPAD_NONE
#include "nuklear_console/vendor/nuklear_gamepad/nuklear_gamepad.h"
#define NK_CONSOLE_IMPLEMENTATION
#include "nuklear_console/nuklear_console.h"

#include <cstdlib> // for malloc and free

void Menu::nk_cairo_render(cairo_t *cr, struct nk_context *ctx) const {
    const struct nk_command *cmd;

    // Iterate over all commands in the Nuklear command buffer
    nk_foreach(cmd, ctx) {
        switch (cmd->type) {
            case NK_COMMAND_NOP:
                break;
            case NK_COMMAND_LINE: {
                const struct nk_command_line *line = (const struct nk_command_line *)cmd;
                cairo_set_source_rgb(cr,
                                     line->color.r / 255.0,
                                     line->color.g / 255.0,
                                     line->color.b / 255.0);
                cairo_set_line_width(cr, line->line_thickness);
                cairo_move_to(cr, line->begin.x, line->begin.y);
                cairo_line_to(cr, line->end.x, line->end.y);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_RECT: {
                const struct nk_command_rect *rect = (const struct nk_command_rect *)cmd;
                cairo_set_source_rgb(cr,
                                     rect->color.r / 255.0,
                                     rect->color.g / 255.0,
                                     rect->color.b / 255.0);
                cairo_set_line_width(cr, rect->line_thickness);
                cairo_rectangle(cr, rect->x, rect->y, rect->w, rect->h);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_RECT_FILLED: {
                const struct nk_command_rect_filled *rect = (const struct nk_command_rect_filled *)cmd;
                cairo_set_source_rgb(cr,
                                     rect->color.r / 255.0,
                                     rect->color.g / 255.0,
                                     rect->color.b / 255.0);
                cairo_rectangle(cr, rect->x, rect->y, rect->w, rect->h);
                cairo_fill(cr);
                break;
            }
            case NK_COMMAND_TEXT: {
                const struct nk_command_text *text = (const struct nk_command_text *)cmd;
                cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, text->font->height);
                cairo_set_source_rgb(cr,
                                     text->foreground.r / 255.0,
                                     text->foreground.g / 255.0,
                                     text->foreground.b / 255.0);
                cairo_move_to(cr, text->x, text->y + text->font->height);
                cairo_show_text(cr, text->string);
                break;
            }
            case NK_COMMAND_CIRCLE_FILLED: {
                const struct nk_command_circle_filled *circle = (const struct nk_command_circle_filled *)cmd;
                cairo_set_source_rgb(cr,
                                     circle->color.r / 255.0,
                                     circle->color.g / 255.0,
                                     circle->color.b / 255.0);
                cairo_arc(cr,
                          circle->x + circle->w / 2.0,
                          circle->y + circle->h / 2.0,
                          circle->w / 2.0, 0, 2 * M_PI);
                cairo_fill(cr);
                break;
            }
            case NK_COMMAND_CIRCLE: {
                const struct nk_command_circle *circle = (const struct nk_command_circle *)cmd;
                cairo_set_source_rgb(cr,
                                     circle->color.r / 255.0,
                                     circle->color.g / 255.0,
                                     circle->color.b / 255.0);
                cairo_set_line_width(cr, circle->line_thickness);
                cairo_arc(cr,
                          circle->x + circle->w / 2.0,
                          circle->y + circle->h / 2.0,
                          circle->w / 2.0, 0, 2 * M_PI);
                cairo_stroke(cr);
                break;
            }
            case NK_COMMAND_IMAGE: {
                const struct nk_command_image *image = (const struct nk_command_image *)cmd;
                // Assuming Cairo supports loading the image into a cairo_surface_t
                cairo_surface_t *img = (cairo_surface_t *)image->img.handle.ptr;
                cairo_set_source_surface(cr, img, image->x, image->y);
                cairo_rectangle(cr, image->x, image->y, image->w, image->h);
                cairo_fill(cr);
                break;
            }
            case NK_COMMAND_SCISSOR:
            {
                const struct nk_command_scissor *s = (const struct nk_command_scissor *)cmd;
                cairo_reset_clip(cr);
                if (s->x >= 0) {
                    cairo_rectangle(cr, s->x - 1, s->y - 1, s->w + 2, s->h + 2);
                    cairo_clip(cr);
                }
                break;
            }
            default:
                SPDLOG_INFO("Unsupportet Nuklear command {}",cmd->type);
                break;
        }
    }
}

void Menu::initMenu() {

    // Set up the console within the Nuklear context
    console = nk_console_init(&ctx);

    // Add some widgets
    //nk_console_button(console, "New Game");
    // nk_console* options = nk_console_button(console, "Options");
    // {
    //     nk_console_button(options, "Some cool option!");
    //     nk_console_button(options, "Option #2");
    //     nk_console_button_onclick(options, "Back", nk_console_button_back);
    // }
    // nk_console_button(console, "Load Game");
    // nk_console_button(console, "Save Game");
    // Sliders
    nk_console_slider_int(console, "Slider Int", 0, &i, 20, 1);
    nk_console_slider_int(console, "Slider Int2", 0, &j, 20, 1);

}

void Menu::drawMenu(struct modeset_buf *buf) {

	cairo_t* cr;
	cairo_surface_t *surface;
	surface = cairo_image_surface_create_for_data(buf->map, CAIRO_FORMAT_ARGB32, buf->width, buf->height, buf->stride);
	cr = cairo_create (surface);

	// https://www.cairographics.org/FAQ/#clear_a_surface
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0); // Transparent black (alpha = 0)
    cairo_paint(cr);

    nk_console_render_window(console, "nuklear_console", nk_rect(200, 200, 800, 600), NK_WINDOW_TITLE);

    // Render Nuklear commands using Cairo
    nk_cairo_render(cr, &ctx);

    // Cleanup
    nk_clear(&ctx);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void Menu::handleInput(char input) {
    nk_input_begin(&ctx);  // Start input processing
    switch (input) {
        case 'e':
            nk_input_key(&ctx, NK_KEY_ENTER, true);
            break;
        case 'w': 
            nk_input_key(&ctx, NK_KEY_UP, true);
            break;  
        case 's': 
            nk_input_key(&ctx, NK_KEY_DOWN, true);
            break;  
        case 'a': 
            nk_input_key(&ctx, NK_KEY_LEFT, true);
            break;  
        case 'd': 
            nk_input_key(&ctx, NK_KEY_RIGHT, true);
            break;  
        default:
            SPDLOG_INFO("Unhandled key: {}", std::string(1, input));  // Log unknown input
            break;
    }

    nk_input_end(&ctx);  // End input processing
}

void Menu::releaseKeys(void) {
    nk_input_begin(&ctx);  // Start input processing
    nk_input_key(&ctx, NK_KEY_ENTER, false);
    nk_input_key(&ctx, NK_KEY_UP, false);
    nk_input_key(&ctx, NK_KEY_DOWN, false);
    nk_input_key(&ctx, NK_KEY_LEFT, false);
    nk_input_key(&ctx, NK_KEY_RIGHT, false);
    nk_input_end(&ctx);  // End input processing
}
