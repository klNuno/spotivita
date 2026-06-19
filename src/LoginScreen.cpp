#include "LoginScreen.h"
#include "Gui.h"

// Spotify killed username/password login in 2024, so the Vita can no longer log
// in with typed credentials. Instead it advertises itself as a Spotify Connect
// device (mDNS, started from main) and waits for the user's phone to hand over
// an authentication blob. This screen just tells the user how to do that.
void LoginScreen::draw() {
    // top spacer
    ImGui::Dummy(ImVec2(0.0f, 42.0f));

    // cspot logo
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + logo_width);
    ImGui::Image(reinterpret_cast<void*>(logo_tex), ImVec2(logo_width, logo_height));
    ImGui::Dummy(ImVec2(0.0f, 28.0f));

    TextCentered("Waiting for Spotify Connect");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    TextCentered("1. Put your phone on the same Wi-Fi as the Vita");
    TextCentered("2. Open Spotify on your phone");
    TextCentered("3. Tap the Connect (devices) icon");
    TextCentered("4. Pick \"PS Vita (CSpot)\" in the device list");

    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    TextCentered("Spotify Premium required.");
}

LoginScreen::LoginScreen(GUI *gui) : Screen(gui) {
    LoadTextureFromFile("icon_alpha.png", &logo_tex, &logo_width, &logo_height);
}

LoginScreen::~LoginScreen() {
    // free cspot logo texture
    glDeleteTextures(1, &logo_tex);
}
