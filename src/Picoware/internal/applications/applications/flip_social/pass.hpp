#pragma once
#include "../../../../internal/system/view.hpp"
#include "../../../../internal/system/view_manager.hpp"
#include "../../../../internal/applications/applications/flip_social/utils.hpp"
using namespace Picoware;

// start must return true (see user.hpp) or switchTo() bounces straight back.
static bool flipSocialPassStart(ViewManager *viewManager) { return true; }

// Direct-tap entry of the FlipSocial password (masked, with SHOW/HIDE toggle).
static void flipSocialPassRun(ViewManager *viewManager)
{
    TFT_eSPI *tft = viewManager->getDraw()->display->getTFT();
    char buf[64];
    buf[0] = '\0';
    strncpy(buf, flipSocialUtilsLoadPasswordFromFlash(viewManager).c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    bool ok = touchKeyboardInput(*tft, viewManager->getForegroundColor(),
                                 viewManager->getBackgroundColor(), buf, sizeof(buf),
                                 "Password:", true);
    if (ok)
        flipSocialUtilsSavePasswordToFlash(viewManager, String(buf));

    viewManager->getInputManager()->reset(true);
    viewManager->back();
}

const PROGMEM View flipSocialPasswordView = View("FlipSocialPassword", flipSocialPassRun, flipSocialPassStart, nullptr);