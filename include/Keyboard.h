#pragma once

#include <functional>
#include <string>

// Non-blocking system IME dialog. The old GetText() ran its own render loop from
// inside an ImGui frame (a button handler), so the frame stayed open for as long
// as the keyboard was up. Now the dialog is opened, the GUI loop keeps running
// and lets vita2d draw the system dialog over each frame while it is visible, and the result
// arrives through the callback.
namespace Keyboard {

// Opens the dialog; onDone gets the text, or is not called on cancel.
// Returns false if a dialog is already open or the system refused it.
bool Open(const std::string &title, const std::string &initial,
          std::function<void(const std::string &)> onDone);

bool Active();

// GUI loop, after each swap while Active().
void Poll();

}  // namespace Keyboard
