#pragma once

#include "Screen.h"
#include "GuiUtils.h"

class LoginScreen: public Screen {
 public:
    explicit LoginScreen(GUI *gui);
    ~LoginScreen();
    void draw();
 private:
    int logo_width = 0;
    int logo_height = 0;
    GLuint logo_tex = 0;
};
