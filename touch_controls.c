/*
TOUCH_CONTROLS.C

On-screen controls for the Android build.

An Xbox controller is drawn over the game: the left stick (move), A B X Y,
the triggers (LT throws a grenade, RT fires), Black, White, the stick clicks
(L3 crouches), the D-pad, Start and Back. Pressing a control presses the
button of the controller on port 0; xinput_sdl.c merges them with the
physical gamepad's.

There is no stick for the view: a finger dragged anywhere on the screen that
is not on a control turns it, like the mouse does (xinput_sdl.c adds the drag
to the mouse's motion, so the aim is direct).

The EDIT button (top right) opens the editor: drag a control to move it, tap
it to select it, and the buttons in the middle change its size, hide it or
show it again, set the opacity of the controls and the touch sensitivity of
the view, or restore the defaults. DONE saves the layout in touch_controls.txt
next to config.toml.

Everything is drawn in pixels with one small shader, just before the frame is
shown (platform_video_swap); the renderer sets all its state again after
that, so only the vertex array, the program and the array buffer the game had
bound are put back.
*/

#ifdef HALO_ANDROID

#include "platform.h"
#include "gl.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "touch_controls.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ---------- the controls */

enum control_id
{
	C_STICK,
	C_A, C_B, C_X, C_Y,
	C_LT, C_RT,
	C_BLACK, C_WHITE,
	C_L3, C_R3,
	C_START, C_BACK,
	C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT,
	C_KEYBOARD, C_COPY1, C_COPY2, C_COPY3, C_COPY4,
	C_COUNT
};

struct control
{
	const char *label;
	int source; /* -1 for normal/special control; duplicate source for COPY controls */
	int image_slot; /* 0 = generated button, 1..8 = touch_button_N.bmp */
	/* the centre as a fraction of the screen, and the diameter as a fraction
	of its height (the saved layout keeps these, so it fits any screen) */
	float x, y, size;
	int visible;
};

#define SIZE_MINIMUM 0.05f
#define SIZE_MAXIMUM 0.60f

static struct control controls[C_COUNT] =
{
	{ "", -1, 0, 0, 0, 0, 1 },
	{ "A", -1, 0, 0, 0, 0, 1 }, { "B", -1, 0, 0, 0, 0, 1 }, { "X", -1, 0, 0, 0, 0, 1 }, { "Y", -1, 0, 0, 0, 0, 1 },
	{ "LT", -1, 0, 0, 0, 0, 1 }, { "RT", -1, 0, 0, 0, 0, 1 },
	{ "BLK", -1, 0, 0, 0, 0, 1 }, { "WHT", -1, 0, 0, 0, 0, 1 },
	{ "L3", -1, 0, 0, 0, 0, 1 }, { "R3", -1, 0, 0, 0, 0, 1 },
	{ "STA", -1, 0, 0, 0, 0, 1 }, { "BCK", -1, 0, 0, 0, 0, 1 },
	{ "", -1, 0, 0, 0, 0, 1 }, { "", -1, 0, 0, 0, 0, 1 }, { "", -1, 0, 0, 0, 0, 1 }, { "", -1, 0, 0, 0, 0, 1 },
	{ "KEY", -1, 0, 0, 0, 0, 1 }, { "C1", -1, 0, 0, 0.80f, 0.13f, 0 }, { "C2", -1, 0, 0, 0.80f, 0.13f, 0 }, { "C3", -1, 0, 0, 0.80f, 0.13f, 0 }, { "C4", -1, 0, 0, 0.80f, 0.13f, 0 },
};

static float opacity = 0.55f;
/* mouse pixels per pixel of finger drag */
static float look_scale = 1.5f;
static int fps_enabled;
static double fps_value;
static Uint64 fps_last_ns;
static unsigned fps_frames;
static int keyboard_active;

#define OPACITY_MINIMUM 0.15f
#define OPACITY_MAXIMUM 1.0f
#define LOOK_MINIMUM 0.25f
#define LOOK_MAXIMUM 6.0f

static void place(int id, float x, float y, float size)
{
	controls[id].x = x;
	controls[id].y = y;
	controls[id].size = size;
	controls[id].visible = 1;
}

/* the layout of an Xbox controller held in two hands, for a screen of the
given width over height */
static void defaults(float aspect)
{
	const float spread = 0.115f;
	const float dpad = 0.075f;
	float cx = 1.0f - 0.50f / aspect, cy = 0.70f;
	float dx = 0.30f, dy = 0.20f;

	place(C_STICK, 0.50f / aspect, 0.70f, 0.40f);
	place(C_A, cx, cy + spread, 0.13f);
	place(C_B, cx + spread / aspect, cy, 0.13f);
	place(C_X, cx - spread / aspect, cy, 0.13f);
	place(C_Y, cx, cy - spread, 0.13f);
	place(C_LT, 0.20f / aspect, 0.38f, 0.15f);
	place(C_RT, 1.0f - 0.20f / aspect, 0.38f, 0.15f);
	place(C_WHITE, 0.20f / aspect, 0.22f, 0.11f);
	place(C_BLACK, 1.0f - 0.20f / aspect, 0.22f, 0.11f);
	place(C_L3, 0.83f / aspect, 0.90f, 0.10f);
	place(C_R3, 1.0f - 0.83f / aspect, 0.90f, 0.10f);
	place(C_START, 0.54f, 0.08f, 0.075f);
	place(C_BACK, 0.46f, 0.08f, 0.075f);
	place(C_DUP, dx, dy - dpad, dpad);
	place(C_DDOWN, dx, dy + dpad, dpad);
	place(C_DLEFT, dx - dpad / aspect, dy, dpad);
	place(C_DRIGHT, dx + dpad / aspect, dy, dpad);
	place(C_KEYBOARD, 0.50f, 0.94f, 0.075f);
	controls[C_KEYBOARD].visible = 1;
	for (int copy = C_COPY1; copy <= C_COPY4; copy++)
	{
		controls[copy].visible = 0;
		controls[copy].source = -1;
	}
	opacity = 0.55f;
	look_scale = 1.5f;
}

/* ---------- state */

#define MAX_FINGERS 10
#define ROLE_NONE (-1)
#define ROLE_LOOK (-2)
#define ROLE_UI (-3)

struct finger
{
	int active;
	SDL_FingerID id;
	/* a control (control_id), or ROLE_* */
	int role;
	/* where it was last, in pixels */
	float x, y;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct finger fingers[MAX_FINGERS];
static int screen_w = 1, screen_h = 1;
static int loaded;
static int editing;
static int selected = -1;
/* the controls held down, one bit each */
static unsigned long held;
/* the left stick, -1 to 1, y down the screen */
static float stick_x, stick_y;
/* the drag of the view not yet taken, in pixels */
static float look_x, look_y;

static unsigned long bit(int id)
{
	return 1ul << id;
}

static float clampf(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

/* ---------- saved layout */

static void layout_path(char *path, size_t size)
{
	char folder[1024];

	config_folder(folder, sizeof(folder));
	snprintf(path, size, "%stouch_controls.txt", folder);
}

static void layout_save(void)
{
	char path[1100];
	FILE *file;
	int id;

	layout_path(path, sizeof(path));
	file = fopen(path, "wb");
	if (!file)
	{
		platform_log("cannot write %s", path);
		return;
	}
	fprintf(file, "opacity %.3f\n", (double)opacity);
	fprintf(file, "look %.3f\n", (double)look_scale);
	fprintf(file, "fps %d\n", fps_enabled);
	for (id = 0; id < C_COUNT; id++)
		fprintf(file, "c %d %.4f %.4f %.4f %d %d\n", id, (double)controls[id].x, (double)controls[id].y,
			(double)controls[id].size, controls[id].visible, controls[id].source);
		fprintf(file, "i %d %d\n", id, controls[id].image_slot);
	fclose(file);
}

static void layout_load(void)
{
	char path[1100];
	size_t size = 0;
	char *text, *line, *next;

	layout_path(path, sizeof(path));
	text = config_file_read(path, &size);
	if (!text)
		return;
	for (line = text; line && *line; line = next)
	{
		int id, visible, source = -1;
		float x, y, scale;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (sscanf(line, "opacity %f", &scale) == 1)
			opacity = clampf(scale, OPACITY_MINIMUM, OPACITY_MAXIMUM);
		else if (sscanf(line, "look %f", &scale) == 1)
			look_scale = clampf(scale, LOOK_MINIMUM, LOOK_MAXIMUM);
		else if (sscanf(line, "fps %d", &id) == 1)
			fps_enabled = id != 0;
		else if (sscanf(line, "i %d %d", &id, &source) == 2 && id >= 0 && id < C_COUNT)
			controls[id].image_slot = source >= 0 && source <= 8 ? source : 0;
		else if (sscanf(line, "c %d %f %f %f %d %d", &id, &x, &y, &scale, &visible, &source) == 6 && id >= 0 && id < C_COUNT)
		{
			controls[id].x = clampf(x, 0.0f, 1.0f);
			controls[id].y = clampf(y, 0.0f, 1.0f);
			controls[id].size = clampf(scale, SIZE_MINIMUM, SIZE_MAXIMUM);
			controls[id].visible = visible != 0;
			controls[id].source = (source >= 0 && source < C_COUNT) ? source : -1;
		}
		else if (sscanf(line, "c %d %f %f %f %d", &id, &x, &y, &scale, &visible) == 5 && id >= 0 && id < C_COUNT)
		{
			controls[id].x = clampf(x, 0.0f, 1.0f);
			controls[id].y = clampf(y, 0.0f, 1.0f);
			controls[id].size = clampf(scale, SIZE_MINIMUM, SIZE_MAXIMUM);
			controls[id].visible = visible != 0;
		}
	}
	free(text);
}

static void screen_update(void)
{
	int width = 0, height = 0;

	platform_video_drawable_size(&width, &height);
	screen_w = width > 0 ? width : 1;
	screen_h = height > 0 ? height : 1;
	if (!loaded && width > 1 && height > 1)
	{
		loaded = 1;
		defaults((float)screen_w / (float)screen_h);
		layout_load();
	}
}

/* ---------- geometry */

static float control_x(int id) { return controls[id].x * (float)screen_w; }
static float control_y(int id) { return controls[id].y * (float)screen_h; }
static float control_radius(int id) { return controls[id].size * (float)screen_h * 0.5f; }

/* the EDIT button */
static float gear_radius(void) { return 0.045f * (float)screen_h; }
static float gear_x(void) { return (float)screen_w - 0.075f * (float)screen_h; }
static float gear_y(void) { return 0.075f * (float)screen_h; }

static int control_at(float px, float py)
{
	int id;

	for (id = C_COUNT - 1; id >= 0; id--)
	{
		float reach = control_radius(id) * (id == C_STICK ? 1.25f : 1.1f);

		if (!controls[id].visible && !editing)
			continue;
		if (hypotf(px - control_x(id), py - control_y(id)) <= reach)
			return id;
	}
	return -1;
}

static int gear_at(float px, float py)
{
	return hypotf(px - gear_x(), py - gear_y()) <= gear_radius() * 1.6f;
}

/* ---------- the editor's buttons */

enum tool
{
	T_SIZE_UP, T_SIZE_DOWN, T_SHOW, T_OPACITY_UP, T_OPACITY_DOWN,
	T_LOOK_UP, T_LOOK_DOWN, T_FPS, T_DUPLICATE, T_IMAGE, T_KEYBOARD, T_RESET, T_DONE,
	T_COUNT
};

struct tool_button
{
	float x0, y0, x1, y1;
	const char *label;
};

static float tool_scale(void)
{
	float scale = floorf((float)screen_h * 0.0048f);

	return scale < 2.0f ? 2.0f : scale;
}

static float text_width(const char *text, float scale)
{
	size_t length = strlen(text);

	return length ? ((float)length * 6.0f - 1.0f) * scale : 0.0f;
}

/* the top of the column of buttons, and the room around them */
static float tool_top(void)
{
	const float pad = 0.022f * (float)screen_h, gap = 0.012f * (float)screen_h;
	float height = 7.0f * tool_scale() + 2.0f * pad;

	return ((float)screen_h - (5.0f * height + 4.0f * gap)) * 0.5f;
}

static void tool_layout(struct tool_button *buttons)
{
	static const int column[T_COUNT] = { 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2 };
	static const int row[T_COUNT] = { 0, 1, 2, 3, 4, 0, 1, 2, 3, 0, 1, 2, 3, 4 };
	const float scale = tool_scale();
	const float pad = 0.022f * (float)screen_h, gap = 0.012f * (float)screen_h;
	const float height = 7.0f * scale + 2.0f * pad;
	const float width = text_width("SIZE-", scale) + 2.0f * pad;
	const float top = tool_top();
	int tool;

	for (tool = 0; tool < T_COUNT; tool++)
	{
		float x = (float)screen_w * 0.5f + ((float)column[tool] - 1.0f) * (width + gap);
		float y = top + (float)row[tool] * (height + gap);

		buttons[tool].x0 = x;
		buttons[tool].y0 = y;
		buttons[tool].x1 = x + width;
		buttons[tool].y1 = y + height;
	}
	buttons[T_SIZE_UP].label = "SIZE+";
	buttons[T_SIZE_DOWN].label = "SIZE-";
	buttons[T_SHOW].label = selected >= 0 && !controls[selected].visible ? "SHOW" : "HIDE";
	buttons[T_OPACITY_UP].label = "OPAC+";
	buttons[T_OPACITY_DOWN].label = "OPAC-";
	buttons[T_LOOK_UP].label = "LOOK+";
	buttons[T_LOOK_DOWN].label = "LOOK-";
	buttons[T_FPS].label = fps_enabled ? "FPS ON" : "FPS OFF";
	buttons[T_DUPLICATE].label = "DUP";
	buttons[T_IMAGE].label = selected >= 0 ? "IMG+" : "IMAGE";
	buttons[T_KEYBOARD].label = keyboard_active ? "KEY ON" : "KEY OFF";
	buttons[T_RESET].label = "RESET";
	buttons[T_DONE].label = "DONE";
}

static int tool_at(float px, float py)
{
	struct tool_button buttons[T_COUNT];
	int tool;

	tool_layout(buttons);
	for (tool = 0; tool < T_COUNT; tool++)
	{
		if (px >= buttons[tool].x0 && px <= buttons[tool].x1 && py >= buttons[tool].y0 && py <= buttons[tool].y1)
			return tool;
	}
	return -1;
}

static void editing_set(int on)
{
	int index;

	if (on == editing)
		return;
	editing = on;
	selected = -1;
	/* what is held is let go of, and the drag not taken is dropped */
	for (index = 0; index < MAX_FINGERS; index++)
		fingers[index].role = fingers[index].active ? ROLE_UI : ROLE_NONE;
	held = 0;
	stick_x = stick_y = 0.0f;
	look_x = look_y = 0.0f;
	if (!on)
		layout_save();
}

static void tool_press(int tool)
{
	switch (tool)
	{
		case T_SIZE_UP:
			if (selected >= 0)
				controls[selected].size = clampf(controls[selected].size * 1.08f, SIZE_MINIMUM, SIZE_MAXIMUM);
			break;
		case T_SIZE_DOWN:
			if (selected >= 0)
				controls[selected].size = clampf(controls[selected].size / 1.08f, SIZE_MINIMUM, SIZE_MAXIMUM);
			break;
		case T_SHOW:
			if (selected >= 0)
				controls[selected].visible = !controls[selected].visible;
			break;
		case T_OPACITY_UP:
			opacity = clampf(opacity + 0.05f, OPACITY_MINIMUM, OPACITY_MAXIMUM);
			break;
		case T_OPACITY_DOWN:
			opacity = clampf(opacity - 0.05f, OPACITY_MINIMUM, OPACITY_MAXIMUM);
			break;
		case T_LOOK_UP:
			look_scale = clampf(look_scale + 0.25f, LOOK_MINIMUM, LOOK_MAXIMUM);
			break;
		case T_LOOK_DOWN:
			look_scale = clampf(look_scale - 0.25f, LOOK_MINIMUM, LOOK_MAXIMUM);
			break;
		case T_FPS:
			fps_enabled = !fps_enabled;
			fps_frames = 0; fps_last_ns = 0;
			break;
		case T_DUPLICATE:
			if (selected >= 0)
			{
				int copy;
				for (copy = C_COPY1; copy <= C_COPY4; copy++)
					if (!controls[copy].visible) break;
				if (copy <= C_COPY4)
				{
					controls[copy] = controls[selected];
					controls[copy].source = selected >= C_COPY1 && selected <= C_COPY4 ? controls[selected].source : selected;
					controls[copy].visible = 1;
					controls[copy].x = clampf(controls[copy].x + 0.04f, 0.0f, 1.0f);
					controls[copy].y = clampf(controls[copy].y + 0.04f, 0.0f, 1.0f);
					selected = copy;
				}
			}
			break;
		case T_IMAGE:
			if (selected >= 0)
				controls[selected].image_slot = (controls[selected].image_slot + 1) % 9;
			break;
		case T_KEYBOARD:
			keyboard_active = !keyboard_active;
			platform_text_typing(keyboard_active);
			break;
		case T_RESET:
			keyboard_active = 0;
			defaults((float)screen_w / (float)screen_h);
			selected = -1;
			break;
		case T_DONE:
			editing_set(0);
			break;
	}
}


static int control_input_id(int id)
{
	if (id >= C_COPY1 && id <= C_COPY4)
		return controls[id].source >= 0 ? controls[id].source : -1;
	return id;
}

static void keyboard_toggle(void)
{
	keyboard_active = !keyboard_active;
	platform_text_typing(keyboard_active);
}

/* ---------- fingers */

static struct finger *finger_find(SDL_FingerID id)
{
	int index;

	for (index = 0; index < MAX_FINGERS; index++)
	{
		if (fingers[index].active && fingers[index].id == id)
			return &fingers[index];
	}
	return NULL;
}

static struct finger *finger_new(SDL_FingerID id)
{
	int index;

	for (index = 0; index < MAX_FINGERS; index++)
	{
		if (!fingers[index].active)
		{
			memset(&fingers[index], 0, sizeof(fingers[index]));
			fingers[index].active = 1;
			fingers[index].id = id;
			fingers[index].role = ROLE_NONE;
			return &fingers[index];
		}
	}
	return NULL;
}

/* what the fingers hold: the buttons down, and the stick */
static void fingers_recompute(void)
{
	int index;

	held = 0;
	stick_x = stick_y = 0.0f;
	for (index = 0; index < MAX_FINGERS; index++)
	{
		const struct finger *finger = &fingers[index];

		if (!finger->active || finger->role < 0 || finger->role >= C_COUNT)
			continue;
		held |= bit(finger->role);
		if (finger->role == C_STICK)
		{
			float radius = control_radius(C_STICK);
			float x = (finger->x - control_x(C_STICK)) / radius;
			float y = (finger->y - control_y(C_STICK)) / radius;
			float length = hypotf(x, y);

			if (length > 1.0f)
			{
				x /= length;
				y /= length;
			}
			stick_x = x;
			stick_y = y;
		}
	}
}

int touch_controls_event(const SDL_Event *event)
{
	const SDL_TouchFingerEvent *touch = &event->tfinger;
	struct finger *finger;
	float x, y;

	if (event->type != SDL_EVENT_FINGER_DOWN && event->type != SDL_EVENT_FINGER_MOTION &&
		event->type != SDL_EVENT_FINGER_UP && event->type != SDL_EVENT_FINGER_CANCELED)
		return keyboard_active;
	pthread_mutex_lock(&lock);
	screen_update();
	x = touch->x * (float)screen_w;
	y = touch->y * (float)screen_h;
	switch (event->type)
	{
		case SDL_EVENT_FINGER_DOWN:
			finger = finger_find(touch->fingerID);
			if (!finger)
				finger = finger_new(touch->fingerID);
			if (!finger)
				break;
			finger->x = x;
			finger->y = y;
			if (gear_at(x, y))
			{
				finger->role = ROLE_UI;
				editing_set(!editing);
			}
			else if (editing)
			{
				int tool = tool_at(x, y);
				int id = tool < 0 ? control_at(x, y) : -1;

				if (tool >= 0)
				{
					finger->role = ROLE_UI;
					tool_press(tool);
				}
				else if (id >= 0)
				{
					/* it follows the finger */
					selected = id;
					finger->role = id;
				}
				else
				{
					selected = -1;
					finger->role = ROLE_UI;
				}
			}
			else
			{
				int id = control_at(x, y);

				if (id == C_KEYBOARD)
				{
					keyboard_toggle();
					finger->role = ROLE_UI;
				}
				else
					finger->role = id >= 0 ? id : ROLE_LOOK;
			}
			break;
		case SDL_EVENT_FINGER_MOTION:
			finger = finger_find(touch->fingerID);
			if (!finger)
				break;
			if (editing)
			{
				if (finger->role >= 0 && finger->role < C_COUNT)
				{
					controls[finger->role].x = clampf(controls[finger->role].x + (x - finger->x) / (float)screen_w, 0.0f, 1.0f);
					controls[finger->role].y = clampf(controls[finger->role].y + (y - finger->y) / (float)screen_h, 0.0f, 1.0f);
				}
			}
			else if (finger->role == ROLE_LOOK)
			{
				look_x += x - finger->x;
				look_y += y - finger->y;
			}
			finger->x = x;
			finger->y = y;
			break;
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_CANCELED:
			finger = finger_find(touch->fingerID);
			if (finger)
				finger->active = 0;
			break;
	}
	fingers_recompute();
	pthread_mutex_unlock(&lock);
	return keyboard_active;
}

/* ---------- into the controller */

void touch_controls_apply(XINPUT_GAMEPAD *pad)
{
	pthread_mutex_lock(&lock);
	if (!editing)
	{
		if (held & bit(C_DUP)) pad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
		if (held & bit(C_DDOWN)) pad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
		if (held & bit(C_DLEFT)) pad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
		if (held & bit(C_DRIGHT)) pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
		if (held & bit(C_START)) pad->wButtons |= XINPUT_GAMEPAD_START;
		if (held & bit(C_BACK)) pad->wButtons |= XINPUT_GAMEPAD_BACK;
		if (held & bit(C_L3)) pad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
		if (held & bit(C_R3)) pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
		{
			int id;
			for (id = C_A; id < C_KEYBOARD; id++)
			{
				int source = control_input_id(id);
				if (source >= C_A && source <= C_RT && (held & bit(id)))
					pad->bAnalogButtons[source == C_A ? XINPUT_GAMEPAD_A : source == C_B ? XINPUT_GAMEPAD_B : source == C_X ? XINPUT_GAMEPAD_X : source == C_Y ? XINPUT_GAMEPAD_Y : source == C_BLACK ? XINPUT_GAMEPAD_BLACK : source == C_WHITE ? XINPUT_GAMEPAD_WHITE : source == C_LT ? XINPUT_GAMEPAD_LEFT_TRIGGER : XINPUT_GAMEPAD_RIGHT_TRIGGER] = 0xff;
			}
			for (id = C_COPY1; id <= C_COPY4; id++)
			{
				int source = control_input_id(id);
				if (source >= C_A && source <= C_RT && (held & bit(id)))
					pad->bAnalogButtons[source == C_A ? XINPUT_GAMEPAD_A : source == C_B ? XINPUT_GAMEPAD_B : source == C_X ? XINPUT_GAMEPAD_X : source == C_Y ? XINPUT_GAMEPAD_Y : source == C_BLACK ? XINPUT_GAMEPAD_BLACK : source == C_WHITE ? XINPUT_GAMEPAD_WHITE : source == C_LT ? XINPUT_GAMEPAD_LEFT_TRIGGER : XINPUT_GAMEPAD_RIGHT_TRIGGER] = 0xff;
			}
		}
		if (stick_x != 0.0f || stick_y != 0.0f)
		{
			/* the screen's y runs down; the stick's runs up */
			pad->sThumbLX = (SHORT)(clampf(stick_x, -1.0f, 1.0f) * 32767.0f);
			pad->sThumbLY = (SHORT)(clampf(-stick_y, -1.0f, 1.0f) * 32767.0f);
		}
	}
	pthread_mutex_unlock(&lock);
}

int touch_controls_look(float *dx, float *dy)
{
	int moved;

	pthread_mutex_lock(&lock);
	*dx = editing ? 0.0f : look_x * look_scale;
	*dy = editing ? 0.0f : look_y * look_scale;
	moved = *dx != 0.0f || *dy != 0.0f;
	look_x = look_y = 0.0f;
	pthread_mutex_unlock(&lock);
	return moved;
}

/* ---------- drawing */

struct color
{
	float r, g, b, a;
};

#define MAX_VERTICES 40000
#define VERTEX_FLOATS 8
#define SEGMENTS 28

static float vertices[MAX_VERTICES * VERTEX_FLOATS];
static int vertex_count;
static float circle_cos[SEGMENTS + 1], circle_sin[SEGMENTS + 1];

static void vertex(float x, float y, struct color c)
{
	float *v;

	if (vertex_count >= MAX_VERTICES)
		return;
	v = &vertices[vertex_count++ * VERTEX_FLOATS];
	v[0] = x;
	v[1] = y;
	v[2] = c.r;
	v[3] = c.g;
	v[4] = c.b;
	v[5] = c.a;
	v[6] = 0.0f;
	v[7] = 0.0f;
}

static void vertex_uv(float x, float y, float u, float vcoord, struct color c)
{
	float *v;
	if (vertex_count >= MAX_VERTICES) return;
	v = &vertices[vertex_count++ * VERTEX_FLOATS];
	v[0] = x; v[1] = y;
	v[2] = c.r; v[3] = c.g; v[4] = c.b; v[5] = c.a;
	v[6] = u; v[7] = vcoord;
}

static void textured_quad(float x0, float y0, float x1, float y1, struct color c)
{
	vertex_uv(x0, y0, 0.0f, 0.0f, c);
	vertex_uv(x1, y0, 1.0f, 0.0f, c);
	vertex_uv(x1, y1, 1.0f, 1.0f, c);
	vertex_uv(x0, y0, 0.0f, 0.0f, c);
	vertex_uv(x1, y1, 1.0f, 1.0f, c);
	vertex_uv(x0, y1, 0.0f, 1.0f, c);
}

static void triangle(float x0, float y0, float x1, float y1, float x2, float y2, struct color c)
{
	vertex(x0, y0, c);
	vertex(x1, y1, c);
	vertex(x2, y2, c);
}

static void rectangle(float x0, float y0, float x1, float y1, struct color c)
{
	triangle(x0, y0, x1, y0, x1, y1, c);
	triangle(x0, y0, x1, y1, x0, y1, c);
}

static void disc(float cx, float cy, float radius, struct color c)
{
	int index;

	for (index = 0; index < SEGMENTS; index++)
		triangle(cx, cy, cx + circle_cos[index] * radius, cy + circle_sin[index] * radius,
			cx + circle_cos[index + 1] * radius, cy + circle_sin[index + 1] * radius, c);
}

static void ring(float cx, float cy, float outer, float thickness, struct color c)
{
	float inner = outer - thickness;
	int index;

	for (index = 0; index < SEGMENTS; index++)
	{
		float ox0 = cx + circle_cos[index] * outer, oy0 = cy + circle_sin[index] * outer;
		float ox1 = cx + circle_cos[index + 1] * outer, oy1 = cy + circle_sin[index + 1] * outer;
		float ix0 = cx + circle_cos[index] * inner, iy0 = cy + circle_sin[index] * inner;
		float ix1 = cx + circle_cos[index + 1] * inner, iy1 = cy + circle_sin[index + 1] * inner;

		triangle(ox0, oy0, ox1, oy1, ix1, iy1, c);
		triangle(ox0, oy0, ix1, iy1, ix0, iy0, c);
	}
}

/* a triangle pointing the way (dx, dy), as the D-pad's arrows */
static void arrow(float cx, float cy, float size, int dx, int dy, struct color c)
{
	float px = (float)-dy, py = (float)dx;

	triangle(cx + (float)dx * size, cy + (float)dy * size,
		cx - (float)dx * size * 0.6f + px * size, cy - (float)dy * size * 0.6f + py * size,
		cx - (float)dx * size * 0.6f - px * size, cy - (float)dy * size * 0.6f - py * size, c);
}

/* ---------- a 5 by 7 font: A to Z, 0 to 9, + - . and the space */

static const unsigned char font[40][7] =
{
	{ 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, /* A */
	{ 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e }, /* B */
	{ 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e }, /* C */
	{ 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e }, /* D */
	{ 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f }, /* E */
	{ 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 }, /* F */
	{ 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f }, /* G */
	{ 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 }, /* H */
	{ 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* I */
	{ 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c }, /* J */
	{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
	{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f }, /* L */
	{ 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
	{ 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, /* N */
	{ 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* O */
	{ 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 }, /* P */
	{ 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d }, /* Q */
	{ 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 }, /* R */
	{ 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e }, /* S */
	{ 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e }, /* U */
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 }, /* V */
	{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 }, /* W */
	{ 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 }, /* X */
	{ 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 }, /* Y */
	{ 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f }, /* Z */
	{ 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }, /* 0 */
	{ 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, /* 1 */
	{ 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f }, /* 2 */
	{ 0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e }, /* 3 */
	{ 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, /* 4 */
	{ 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e }, /* 5 */
	{ 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, /* 6 */
	{ 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
	{ 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e }, /* 8 */
	{ 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }, /* 9 */
	{ 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 }, /* + */
	{ 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 }, /* - */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c }, /* . */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* space */
};

static int glyph_index(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a';
	if (c >= '0' && c <= '9') return 26 + (c - '0');
	if (c == '+') return 36;
	if (c == '-') return 37;
	if (c == '.') return 38;
	return 39;
}

/* the text with its top left corner at (x, y), a font pixel being scale
pixels wide; a row of lit pixels is one rectangle */
static void text_draw(float x, float y, float scale, const char *text, struct color c)
{
	for (; *text; text++, x += 6.0f * scale)
	{
		const unsigned char *rows = font[glyph_index(*text)];
		int row, column;

		for (row = 0; row < 7; row++)
		{
			for (column = 0; column < 5; column++)
			{
				int start = column;

				if (!(rows[row] & (0x10 >> column)))
					continue;
				while (column + 1 < 5 && (rows[row] & (0x10 >> (column + 1))))
					column++;
				rectangle(x + (float)start * scale, y + (float)row * scale,
					x + (float)(column + 1) * scale, y + (float)(row + 1) * scale, c);
			}
		}
	}
}

/* the text centred on (cx, cy), no wider than max_width (0: any) */
static void text_centered(float cx, float cy, float scale, float max_width, const char *text, struct color c)
{
	if (max_width > 0.0f && text_width(text, scale) > max_width)
		scale = max_width / text_width(text, 1.0f);
	scale = roundf(scale) >= 1.0f ? roundf(scale) : scale;
	text_draw(cx - text_width(text, scale) * 0.5f, cy - 3.5f * scale, scale, text, c);
}

/* ---------- the controls, drawn */

static struct color color_make(float r, float g, float b, float a)
{
	struct color c;

	c.r = r;
	c.g = g;
	c.b = b;
	c.a = a;
	return c;
}

static struct color control_color(int id)
{
	switch (id)
	{
		case C_A: return color_make(0.20f, 0.72f, 0.25f, 1.0f);
		case C_B: return color_make(0.85f, 0.20f, 0.20f, 1.0f);
		case C_X: return color_make(0.20f, 0.40f, 0.90f, 1.0f);
		case C_Y: return color_make(0.92f, 0.78f, 0.15f, 1.0f);
		case C_BLACK: return color_make(0.08f, 0.08f, 0.08f, 1.0f);
		case C_WHITE: return color_make(0.85f, 0.85f, 0.85f, 1.0f);
		default: return color_make(0.35f, 0.40f, 0.45f, 1.0f);
	}
}

static void control_draw(int id)
{
	float cx = control_x(id), cy = control_y(id), radius = control_radius(id);
	int down = (held & bit(id)) != 0;
	float alpha = controls[id].visible ? opacity : 0.18f;
	struct color body = control_color((id >= C_COPY1 && id <= C_COPY4 && controls[id].source >= 0) ? controls[id].source : id), white = color_make(1.0f, 1.0f, 1.0f, 1.0f);
	float thickness = radius * 0.08f > 2.0f ? radius * 0.08f : 2.0f;

	if (down)
		alpha = clampf(alpha + 0.35f, 0.0f, 1.0f);
	if (id == C_STICK)
	{
		float knob = radius * 0.40f;

		body = color_make(0.12f, 0.14f, 0.16f, alpha * 0.6f);
		white.a = alpha;
		disc(cx, cy, radius, body);
		ring(cx, cy, radius, thickness, white);
		white.a = clampf(alpha + 0.2f, 0.0f, 1.0f);
		disc(cx + stick_x * (radius - knob), cy + stick_y * (radius - knob), knob, white);
	}
	else if (id >= C_DUP && id <= C_DRIGHT)
	{
		int dx = id == C_DLEFT ? -1 : id == C_DRIGHT ? 1 : 0;
		int dy = id == C_DUP ? -1 : id == C_DDOWN ? 1 : 0;

		body = color_make(0.12f, 0.14f, 0.16f, alpha * 0.8f);
		white.a = clampf(alpha + 0.2f, 0.0f, 1.0f);
		disc(cx, cy, radius, body);
		ring(cx, cy, radius, thickness, white);
		arrow(cx, cy, radius * 0.5f, dx, dy, white);
	}
	else
	{
		white.a = clampf(alpha + 0.2f, 0.0f, 1.0f);
		if (id == C_BLACK)
			white.a = alpha;
		body.a = alpha * 0.85f;
		disc(cx, cy, radius, body);
		ring(cx, cy, radius, thickness, white);
		text_centered(cx, cy, radius * 0.17f, radius * 1.4f,
			(id >= C_COPY1 && id <= C_COPY4 && controls[id].source >= 0) ? controls[controls[id].source].label : controls[id].label,
			id == C_WHITE ? color_make(0.1f, 0.1f, 0.1f, white.a) : white);
	}
	if (editing && id == selected)
		ring(cx, cy, radius * 1.12f, thickness * 1.6f, color_make(1.0f, 0.9f, 0.1f, 1.0f));
}

static void number_text(char *buffer, size_t size, const char *name, float value, int percent)
{
	if (percent)
		snprintf(buffer, size, "%s %d", name, (int)(value * 100.0f + 0.5f));
	else
		snprintf(buffer, size, "%s %.2f", name, (double)value);
}

static void editor_draw(void)
{
	struct tool_button buttons[T_COUNT];
	const float scale = tool_scale();
	const float cx = (float)screen_w * 0.5f;
	struct color white = color_make(1.0f, 1.0f, 1.0f, 1.0f);
	char line[64], part[32];
	int tool;

	tool_layout(buttons);
	for (tool = 0; tool < T_COUNT; tool++)
	{
		struct color fill = tool == T_DONE ? color_make(0.15f, 0.55f, 0.20f, 0.92f) :
			tool == T_RESET ? color_make(0.60f, 0.20f, 0.18f, 0.92f) : color_make(0.12f, 0.14f, 0.18f, 0.92f);

		rectangle(buttons[tool].x0, buttons[tool].y0, buttons[tool].x1, buttons[tool].y1, fill);
		text_centered((buttons[tool].x0 + buttons[tool].x1) * 0.5f, (buttons[tool].y0 + buttons[tool].y1) * 0.5f,
			scale, 0.0f, buttons[tool].label, white);
	}
	number_text(line, sizeof(line), "LOOK", look_scale, 0);
	number_text(part, sizeof(part), "  OPAC", opacity, 1);
	strncat(line, part, sizeof(line) - strlen(line) - 1);
	text_centered(cx, tool_top() - 0.06f * (float)screen_h, scale, 0.0f, line, white);
	text_centered(cx, tool_top() + (float)screen_h * 0.46f, scale * 0.8f, 0.0f,
		selected >= 0 ? "DRAG TO MOVE" : "TAP A BUTTON", color_make(1.0f, 0.9f, 0.1f, 1.0f));
}

static void interface_draw(void)
{
	int id;
	float gx, gy, gr;
	struct color white = color_make(1.0f, 1.0f, 1.0f, 1.0f);

	if (editing)
		rectangle(0.0f, 0.0f, (float)screen_w, (float)screen_h, color_make(0.0f, 0.0f, 0.0f, 0.45f));
	for (id = 0; id < C_COUNT; id++)
	{
		if (controls[id].visible || editing)
			control_draw(id);
	}
	gx = gear_x();
	gy = gear_y();
	gr = gear_radius();
	disc(gx, gy, gr, color_make(0.12f, 0.14f, 0.16f, editing ? 0.9f : 0.45f));
	white.a = editing ? 1.0f : 0.6f;
	ring(gx, gy, gr, gr * 0.1f > 2.0f ? gr * 0.1f : 2.0f, white);
	text_centered(gx, gy, gr * 0.14f, gr * 1.5f, editing ? "OK" : "EDIT", white);
	if (fps_enabled)
	{
		char fps_text[32];
		snprintf(fps_text, sizeof(fps_text), "FPS %.0f", fps_value);
		text_draw(0.02f * (float)screen_h, 0.02f * (float)screen_h, tool_scale() * 0.8f, fps_text, white);
	}
	if (editing)
		editor_draw();
}

/* ---------- OpenGL */

static GLuint program, vertex_array, vertex_buffer;
static GLuint image_textures[9];
static unsigned char image_ready[9];
static int image_initialized;
static GLint viewport_uniform = -1;
static GLint texture_uniform = -1;
static GLint use_texture_uniform = -1;
static int gl_state; /* 0: not yet, 1: ready, -1: failed */

static const char *vertex_source =
	"#version 300 es\n"
	"layout(location = 0) in vec2 position;\n"
	"layout(location = 1) in vec4 color;\n"
	"layout(location = 2) in vec2 texcoord_in;\n"
	"uniform vec2 viewport;\n"
	"out vec4 tint;\n"
	"out vec2 texcoord;\n"
	"void main()\n"
	"{\n"
	"\ttint = color;\n"
	"\ttexcoord = texcoord_in;\n"
	"\tgl_Position = vec4(position.x / viewport.x * 2.0 - 1.0, 1.0 - position.y / viewport.y * 2.0, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_source =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 tint;\n"
	"in vec2 texcoord;\n"
	"out vec4 result;\n"
	"uniform sampler2D button_texture;\n"
	"uniform int use_texture;\n"
	"void main()\n"
	"{\n"
	"\tresult = use_texture != 0 ? texture(button_texture, texcoord) * tint : tint;\n"
	"}\n";

static GLuint shader_compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[1024];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("touch controls: cannot compile a shader:\n%s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int gl_initialize(void)
{
	GLuint vertex_shader, fragment_shader;
	GLint status = 0;
	int index;

	for (index = 0; index <= SEGMENTS; index++)
	{
		float angle = (float)index * 6.28318530718f / (float)SEGMENTS;

		circle_cos[index] = cosf(angle);
		circle_sin[index] = sinf(angle);
	}
	vertex_shader = shader_compile(GL_VERTEX_SHADER, vertex_source);
	fragment_shader = shader_compile(GL_FRAGMENT_SHADER, fragment_source);
	if (!vertex_shader || !fragment_shader)
		return 0;
	program = glCreateProgram();
	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[1024];

		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		platform_log("touch controls: cannot link the program:\n%s", log);
		return 0;
	}
	viewport_uniform = glGetUniformLocation(program, "viewport");
	texture_uniform = glGetUniformLocation(program, "button_texture");
	use_texture_uniform = glGetUniformLocation(program, "use_texture");
	glGenVertexArrays(1, &vertex_array);
	glGenBuffers(1, &vertex_buffer);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VERTEX_FLOATS * sizeof(float), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, VERTEX_FLOATS * sizeof(float), (const void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, VERTEX_FLOATS * sizeof(float), (const void *)(6 * sizeof(float)));
	return 1;
}

/* ---------- lightweight BMP loader

   touch_controls.c is part of the Android guest and therefore cannot call
   SDL3's surface/image functions directly.  Keep the custom button images
   self-contained: accept uncompressed 24-bit and 32-bit BMP files and upload
   the pixels straight to OpenGL.
*/
static unsigned short bmp_u16(const unsigned char *p)
{
	return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

static unsigned int bmp_u32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int bmp_s32(const unsigned char *p)
{
	return (int)bmp_u32(p);
}

static unsigned char *load_bmp_rgba(const char *path, int *out_w, int *out_h)
{
	FILE *file;
	unsigned char header[54];
	unsigned int pixel_offset, compression;
	int width, height, top_down, bpp;
	long row_stride;
	unsigned char *pixels = NULL, *row = NULL;
	int y;

	file = fopen(path, "rb");
	if (!file) return NULL;
	if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
		header[0] != 'B' || header[1] != 'M' ||
		bmp_u32(header + 14) < 40)
	{
		fclose(file);
		return NULL;
	}

	pixel_offset = bmp_u32(header + 10);
	width = bmp_s32(header + 18);
	height = bmp_s32(header + 22);
	bpp = (int)bmp_u16(header + 28);
	compression = bmp_u32(header + 30);
	top_down = height < 0;
	if (height < 0) height = -height;

	if (width <= 0 || height <= 0 ||
		(bpp != 24 && bpp != 32) || compression != 0)
	{
		fclose(file);
		return NULL;
	}

	row_stride = ((long)width * bpp + 31L) / 32L * 4L;
	if (row_stride <= 0 || (size_t)width > SIZE_MAX / 4 ||
		(size_t)width * (size_t)height > SIZE_MAX / 4)
	{
		fclose(file);
		return NULL;
	}

	pixels = (unsigned char *)malloc((size_t)width * (size_t)height * 4U);
	row = (unsigned char *)malloc((size_t)row_stride);
	if (!pixels || !row)
	{
		free(pixels);
		free(row);
		fclose(file);
		return NULL;
	}

	if (fseek(file, (long)pixel_offset, SEEK_SET) != 0)
	{
		free(pixels);
		free(row);
		fclose(file);
		return NULL;
	}

	for (y = 0; y < height; y++)
	{
		int dst_y = top_down ? y : (height - 1 - y);
		int x;
		if (fread(row, 1, (size_t)row_stride, file) != (size_t)row_stride)
		{
			free(pixels);
			free(row);
			fclose(file);
			return NULL;
		}
		for (x = 0; x < width; x++)
		{
			const unsigned char *src = row + (size_t)x * (bpp / 8);
			unsigned char *dst = pixels + ((size_t)dst_y * (size_t)width + (size_t)x) * 4U;
			dst[0] = src[2]; /* R */
			dst[1] = src[1]; /* G */
			dst[2] = src[0]; /* B */
			dst[3] = bpp == 32 ? src[3] : 255; /* A */
		}
	}

	free(row);
	fclose(file);
	*out_w = width;
	*out_h = height;
	return pixels;
}

static void image_load_all(void)
{
	char folder[1024], path[1200];
	int slot;
	if (image_initialized) return;
	config_folder(folder, sizeof(folder));
	for (slot = 1; slot <= 8; slot++)
	{
		unsigned char *pixels;
		int width, height;
		snprintf(path, sizeof(path), "%stouch_button_%d.bmp", folder, slot);
		pixels = load_bmp_rgba(path, &width, &height);
		if (!pixels) continue;
		glGenTextures(1, &image_textures[slot]);
		glBindTexture(GL_TEXTURE_2D, image_textures[slot]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		free(pixels);
		image_ready[slot] = 1;
	}
	image_initialized = 1;
}

void touch_controls_draw(void)
{
	if (fps_enabled)
	{
		struct timespec ts;
		Uint64 now;
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		{
			now = (Uint64)ts.tv_sec * 1000000000ULL + (Uint64)ts.tv_nsec;
			if (!fps_last_ns)
				fps_last_ns = now;
			fps_frames++;
			if (now >= fps_last_ns + 1000000000ULL)
			{
				fps_value = (double)fps_frames * 1000000000.0 / (double)(now - fps_last_ns);
				fps_frames = 0;
				fps_last_ns = now;
			}
		}
	}
	GLint previous_vertex_array = 0, previous_program = 0, previous_buffer = 0;
	int count;

	if (gl_state < 0)
		return;
	pthread_mutex_lock(&lock);
	screen_update();
	vertex_count = 0;
	if (gl_state > 0)
		interface_draw();
	count = vertex_count;
	pthread_mutex_unlock(&lock);

	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vertex_array);
	glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
	if (gl_state == 0)
	{
		gl_state = gl_initialize() ? 1 : -1;
		glBindVertexArray((GLuint)previous_vertex_array);
		glUseProgram((GLuint)previous_program);
		glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_buffer);
		return;
	}
	if (!image_initialized) image_load_all();
	if (count <= 0)
		return;
	glViewport(0, 0, screen_w, screen_h);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(program);
	glUniform2f(viewport_uniform, (float)screen_w, (float)screen_h);
	glUniform1i(use_texture_uniform, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)count * VERTEX_FLOATS * sizeof(float)), vertices, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, count);
	if (image_initialized)
	{
		int id;
		glUniform1i(use_texture_uniform, 1);
		glUniform1i(texture_uniform, 0);
		for (id = 0; id < C_COUNT; id++)
		{
			int slot = controls[id].image_slot;
			if (!controls[id].visible || slot <= 0 || slot > 8 || !image_ready[slot]) continue;
			vertex_count = 0;
			textured_quad(control_x(id) - control_radius(id), control_y(id) - control_radius(id),
				control_x(id) + control_radius(id), control_y(id) + control_radius(id),
				color_make(1.0f, 1.0f, 1.0f, opacity));
			glBindTexture(GL_TEXTURE_2D, image_textures[slot]);
			glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)vertex_count * VERTEX_FLOATS * sizeof(float)), vertices, GL_STREAM_DRAW);
			glDrawArrays(GL_TRIANGLES, 0, vertex_count);
		}
		glUniform1i(use_texture_uniform, 0);
	}
	glBindVertexArray((GLuint)previous_vertex_array);
	glUseProgram((GLuint)previous_program);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous_buffer);
}

#else

/* the other ports have no touch controls; an empty file is not valid C */
typedef int touch_controls_is_android_only;

#endif
