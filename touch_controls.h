/*
TOUCH_CONTROLS.H

On-screen controls for the Android build: an Xbox controller drawn over the
game, with the view turned by dragging a finger over the rest of the screen
(see touch_controls.c).
*/

#ifndef __HALO_TOUCH_CONTROLS_H
#define __HALO_TOUCH_CONTROLS_H

#ifdef HALO_ANDROID

#include "platform.h"
#include <SDL3/SDL.h>

/* a finger event (SDL_EVENT_FINGER_*); the rest are ignored */
int touch_controls_event(const SDL_Event *event);
/* adds the pressed buttons and the stick to the controller state of port 0 */
void touch_controls_apply(XINPUT_GAMEPAD *pad);
/* the drag of the view since the last call, in pixels scaled by the touch
sensitivity (what the mouse's motion is); FALSE if there was none */
int touch_controls_look(float *dx, float *dy);
/* draws the controls over the frame; called just before the buffers swap, on
the thread that owns the GL context */
void touch_controls_draw(void);

#endif

#endif
