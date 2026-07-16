#pragma once
#include "Arduino.h"
#include "../../internal/gui/vector.hpp"
#include "../../internal/boards.hpp"
#include "../../internal/system/buttons.hpp"
namespace Picoware
{
    // TouchInput — FT6336 capacitive touch for the Pancake board.
    //
    // Produces two things each run():
    //   * lastButton: a BUTTON_* code derived from screen tap-zones so Picoware's
    //     existing button-navigated views (menus, on-screen keyboard, games) work
    //     unchanged. Zones:  top edge = UP, bottom edge = DOWN, left = LEFT,
    //     right = RIGHT, center = CENTER/OK.
    //   * a raw touch point (x, y in rotated screen coords) + pressed flag for
    //     views that want direct hit-testing (the hybrid touch UI).
    // Assumes Wire.begin() + FT6336 reset has already run (done in the sketch
    // setup via ft6336_init()).
    class TouchInput
    {
    public:
        TouchInput(uint16_t width, uint16_t height, uint8_t rotation);
        void run();                 // poll the panel, update lastButton / point
        void reset();               // clear state + debounce window
        bool isPressed() const noexcept { return pressed; }
        uint16_t x() const noexcept { return px; }
        uint16_t y() const noexcept { return py; }
        Vector point() const noexcept { return Vector(px, py); }

        int lastButton; // BUTTON_* from tap zone this frame, or -1

    private:
        bool readPanel(uint16_t &sx, uint16_t &sy); // raw read + rotation map
        uint16_t w, h;
        uint8_t rot;
        uint16_t px, py;
        bool pressed;
        bool wasDown;
        uint32_t lastMs;
        static const uint32_t DEBOUNCE_MS = 120;
    };

    // Input — a single logical input source. Pancake uses the TouchInput backend;
    // the (pin, button) GPIO backend is retained for boards with physical buttons.
    class Input
    {
    public:
        Input();
        Input(uint8_t pin, uint8_t button, float debounce = 0.05f);
        Input(TouchInput *touch);
        //
        TouchInput *getTouch() const noexcept { return this->touch; }
        uint8_t getButtonAssignment() const noexcept { return this->buttonAssignment; }
        int getLastButton() const noexcept { return this->lastButton; }
        uint8_t getPin() const noexcept { return this->pin; }
        //
        bool isPressed();
        bool isHeld(uint8_t duration = 3);
        void reset();
        void run();
        //
        operator bool() const;

    private:
        uint8_t pin;
        uint8_t buttonAssignment;
        int lastButton;
        float debounce;
        unsigned long startTime;
        float elapsedTime;
        bool wasPressed;
        TouchInput *touch;
    };
}
