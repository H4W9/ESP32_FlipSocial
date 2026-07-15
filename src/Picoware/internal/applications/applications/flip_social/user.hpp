#pragma once
#include "../../../../internal/system/view.hpp"
#include "../../../../internal/system/view_manager.hpp"
#include "../../../../internal/applications/applications/flip_social/utils.hpp"
using namespace Picoware;

// start must return true, otherwise ViewManager::switchTo() immediately calls
// back() (View::start() returns false when the start callback is null).
static bool flipSocialUserStart(ViewManager *viewManager) { return true; }

// Direct-tap entry of the FlipSocial username. Runs the touch keyboard once
// (modal), saves the result, and returns to Settings.
static void flipSocialUserRun(ViewManager *viewManager)
{
    TFT_eSPI *tft = viewManager->getDraw()->display->getTFT();
    char buf[64];
    buf[0] = '\0';
    strncpy(buf, flipSocialUtilsLoadUserFromFlash(viewManager).c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    bool ok = touchKeyboardInput(*tft, viewManager->getForegroundColor(),
                                 viewManager->getBackgroundColor(), buf, sizeof(buf),
                                 "Username:", false);
    if (ok)
        flipSocialUtilsSaveUserToFlash(viewManager, String(buf));

    viewManager->getInputManager()->reset(true);
    viewManager->back();
}

const PROGMEM View flipSocialUserView = View("FlipSocialUser", flipSocialUserRun, flipSocialUserStart, nullptr);