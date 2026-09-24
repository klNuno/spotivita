#include "LoginScreen.h"
#include "Gui.h"
#include "Utils.h"

// Spotify killed username/password login in 2024, so the Vita can no longer log
// in with typed credentials. Instead it advertises itself as a Spotify Connect
// device (mDNS, started from main) and waits for the user's phone to hand over
// an authentication blob. This screen just tells the user how to do that.
void LoginScreen::draw() {
    // About 370 px of content, centred in the 544 px screen: the logo is drawn
    // at 96 px and the steps use tight line spacing.
    const float logo = 96.0f;
    const float scale = logo_height > 0 ? logo / static_cast<float>(logo_height) : 1.0f;
    ImGui::Dummy(ImVec2(0.0f, 56.0f));
    AlignForWidth(logo_width * scale);
    ImGui::Image(Render::tex_id(logo_tex), ImVec2(logo_width * scale, logo));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    ImGui::PushFont(gui->font_bold);
    TextCentered("psvitify");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    TextCentered("Waiting for Spotify Connect");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 4.0f));
    TextCentered("1. Put your phone on the same Wi-Fi as the Vita");
    TextCentered("2. Open Spotify on your phone");
    TextCentered("3. Tap the Connect (devices) icon");
    TextCentered("4. Pick \"psvitify\" in the device list");
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    TextCentered("Spotify Premium required.");
    ImGui::PopStyleColor();
}

LoginScreen::LoginScreen(GUI *gui) : Screen(gui) {
    LoadTextureFromFile("app0:icon_alpha.png", &logo_tex, &logo_width, &logo_height);
}

LoginScreen::~LoginScreen() {
    Render::free_texture(logo_tex);
}
