#pragma once

#include <string>

class GUI;
class Screen {
 public:
    explicit Screen(GUI *gui) : gui(gui) {}
    virtual ~Screen() {}
    virtual void draw() {}
    // Debug server hooks (GUI thread). debugState returns a JSON object body.
    virtual std::string debugState() { return "{}"; }
    virtual bool debugCommand(const std::string &, const std::string &) { return false; }
 protected:
    GUI *gui;
};
