// Window, GL context and presentation.
//
// The game renders into an offscreen framebuffer of the logical (landscape)
// surface size; present() rotates and scales it onto the real window.
#pragma once

#include <cstdint>
#include <functional>

union SDL_Event;

namespace display {
struct Options {
    int surface_height = 0;  // 0 = native window height
    bool rotate = true;      // portrait window, landscape game
    bool flip = false;       // rotate the other way round
};

bool init(const Options &opt);
void shutdown();

int width();   // logical surface size seen by the game
int height();
unsigned game_framebuffer();  // GL name of the offscreen framebuffer
bool rotated();
bool flipped();

void present();

// Convert normalised window coordinates (0..1) to surface coordinates.
void window_to_surface(float nx, float ny, int &sx, int &sy);

// Pump SDL events; returns false when the application should quit.
using EventSink = std::function<void(const SDL_Event &)>;
void pump(const EventSink &sink);
}  // namespace display
