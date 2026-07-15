#pragma once
#include "../../../../internal/system/view.hpp"
#include "../../../../internal/system/view_manager.hpp"
#include "../../../../internal/gui/alert.hpp"
#include "../../../../internal/applications/applications/flip_social/utils.hpp"
using namespace Picoware;

// Compose a FlipSocial post with the touch keyboard, submit it, and show the
// result. Direct-tap (modal) — no on-screen Keyboard widget.
static void flipSocialPostShowAlert(ViewManager *viewManager, const char *message)
{
    viewManager->getDraw()->clear(Vector(0, 0), viewManager->getSize(), viewManager->getBackgroundColor());
    Alert alert(viewManager->getDraw(), message, viewManager->getForegroundColor(), viewManager->getBackgroundColor());
    alert.draw();
    delay(2000);
}

// start must return true (see user.hpp) or switchTo() bounces straight back.
static bool flipSocialPostStart(ViewManager *viewManager) { return true; }

static void flipSocialPostRun(ViewManager *viewManager)
{
    TFT_eSPI *tft = viewManager->getDraw()->display->getTFT();
    char buf[256];
    buf[0] = '\0';
    bool ok = touchKeyboardInput(*tft, viewManager->getForegroundColor(),
                                 viewManager->getBackgroundColor(), buf, sizeof(buf),
                                 "New Post:", false);
    viewManager->getInputManager()->reset(true);

    if (!ok || strlen(buf) == 0)
    {
        viewManager->back();
        return;
    }

    auto draw = viewManager->getDraw();
    draw->clear(Vector(0, 0), viewManager->getSize(), viewManager->getBackgroundColor());
    draw->text(Vector(5, 5), "Posting to FlipSocial...", viewManager->getForegroundColor());
    draw->swap();

    String user = flipSocialUtilsLoadUserFromFlash(viewManager);
    if (user.length() == 0)
    {
        flipSocialPostShowAlert(viewManager, "Username is empty. Set it in Settings.");
        viewManager->back();
        return;
    }

    char command[512];
    snprintf(command, sizeof(command), "{\"username\":\"%s\",\"content\":\"%s\"}", user.c_str(), buf);
    String response = flipSocialHttpRequest(viewManager, "POST",
                                            "https://www.jblanked.com/flipper/api/feed/post/", command);

    if (response.length() == 0 || response.indexOf("ERROR") != -1)
        flipSocialPostShowAlert(viewManager, "Failed to post. Please try again.");
    else
        flipSocialPostShowAlert(viewManager, "Posted!");

    viewManager->back();
}

const PROGMEM View flipSocialPostView = View("FlipSocialPost", flipSocialPostRun, flipSocialPostStart, nullptr);
