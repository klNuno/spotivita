#pragma once

#include "Screen.h"
#include "GuiUtils.h"
#include "Render.h"

class LoginScreen: public Screen {
 public:
    explicit LoginScreen(GUI *gui);
    ~LoginScreen();
    void draw();
 private:
    int logo_width = 0;
    int logo_height = 0;
    vita2d_texture *logo_tex = nullptr;
};
