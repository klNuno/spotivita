#include "Keyboard.h"

#include <psp2/kernel/clib.h>
#include <psp2/ime_dialog.h>
#include <codecvt>
#include <cstring>
#include <locale>
#include <string>
#include <utility>

// Based off of libkdbvita by usineur -> https://github.com/usineur/libkbdvita/blob/master/kbdvita.c
namespace Keyboard {

static bool running = false;
static uint16_t buffer[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static std::u16string title_u16;
static std::function<void(const std::string &)> callback;

bool Open(const std::string &title, const std::string &initial,
          std::function<void(const std::string &)> onDone) {
    if (running) {
        return false;
    }
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> conv;
    // The IME keeps pointers to the title and the buffer until it closes, so
    // both live in statics, not on this stack frame.
    title_u16 = conv.from_bytes(title);
    std::u16string init16 = conv.from_bytes(initial);
    memset(buffer, 0, sizeof(buffer));
    size_t n = init16.size() < SCE_IME_DIALOG_MAX_TEXT_LENGTH ? init16.size()
                                                              : SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    memcpy(buffer, init16.data(), n * sizeof(char16_t));

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;   // every language the system offers
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = 0;
    param.title = reinterpret_cast<const SceWChar16 *>(title_u16.c_str());
    param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    param.initialText = reinterpret_cast<SceWChar16 *>(buffer);
    param.inputTextBuffer = buffer;

    if (sceImeDialogInit(&param) < 0) {
        return false;
    }
    callback = std::move(onDone);
    running = true;
    return true;
}

bool Active() {
    return running;
}

void Poll() {
    if (!running) {
        return;
    }
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        return;
    }
    SceImeDialogResult result;
    sceClibMemset(&result, 0, sizeof(SceImeDialogResult));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    running = false;

    auto cb = std::move(callback);
    callback = nullptr;
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER || result.button == SCE_IME_DIALOG_BUTTON_CLOSE) {
        std::u16string text(reinterpret_cast<const char16_t *>(buffer));
        std::string utf8 =
            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(text);
        if (cb) {
            cb(utf8);
        }
    }
}

}  // namespace Keyboard
