#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/bgapputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <vitaGL.h>

#include <cstring>
#include <cstdarg>
#include <memory>
#include <string>
#include <utility>

#include "Paf.h"
#include <SpircController.h>
#include <JSONObject.h>
#include <ConfigJSON.h>
#include <Logger.h>
#include <HTTPServer.h>
#include <ZeroconfAuthenticator.h>

#include "CliFile.h"
#include "VitaAudioSink.h"
#include "PlaybackScreen.h"
#include "Keyboard.h"
#include "Utils.h"
#include "GuiUtils.h"
#include "Gui.h"
#include "API.h"
#include "Config.h"
#include "Login5.h"

// TODO(michal4132):
// - settings screen
// - make GUI global?
// - fix crash caused by ImGui_ImplVitaGL_Shutdown when in system mode
// - mainline cspot
// - system music volume control
// - system music title, controls

std::shared_ptr<ConfigJSON> configMan;
std::shared_ptr<CliFile> file;
std::shared_ptr<MercuryManager> mercuryManager;
std::shared_ptr<SpircController> spircController;
std::shared_ptr<LoginBlob> blob;
std::shared_ptr<bell::HTTPServer> httpServer;
std::shared_ptr<ZeroconfAuthenticator> zeroconfAuth;

static int watch_id;
static int cspot_id;
static int zeroconf_id;

SceVoid watch_dog(SceSize _args, void *_argp) {
    GUI* gui = *((GUI**)_argp);

#if defined(CRASH_TEST)
    gui->paused = false;
    sceKernelDelayThread(15000000);
    gui->isRunning = false;
    return;
#endif

    SceAppMgrEvent appEvent;

    while (gui->isRunning) {
        sceAppMgrReceiveEvent(&appEvent);
        switch (appEvent.event) {
            case SCE_APP_EVENT_REQUEST_QUIT:
                gui->isRunning = false;
                return;
            case SCE_APP_EVENT_ON_DEACTIVATE:
                gui->paused = true;
                break;
            case SCE_APP_EVENT_ON_ACTIVATE:
                gui->paused = false;
                break;
            default:
                // CSPOT_LOG(debug, "watchdog event: %x", appEvent.event);
                break;
        }
        sceKernelDelayThread(1000000);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }
}

void start_cspot_thread(GUI *gui);

// Spotify Connect (Zeroconf) login. The Vita advertises itself over mDNS and
// serves the /spotify_info endpoint; the user's phone hands us an encrypted
// credentials blob (authType STORED_SPOTIFY_CREDENTIALS, still AP-accepted),
// replacing the dead username/password flow (Spotify removed it in 2024).
int start_zeroconf(SceSize _args, void *_argp) {
    GUI* gui = *((GUI**)_argp);

    httpServer = std::make_shared<bell::HTTPServer>(2137);
    zeroconfAuth = std::make_shared<ZeroconfAuthenticator>(
        [gui](std::shared_ptr<LoginBlob> b) {
            blob = b;
            // The zeroconf blob carries reusable stored credentials. start_cspot
            // persists it once authentication actually succeeds, so subsequent
            // boots auto-login without the phone.
            gui->set_screen(gui->playback_screen);
            start_cspot_thread(gui);
        },
        httpServer);
    zeroconfAuth->registerHandlers();
    httpServer->listen();  // blocks, serving HTTP until the process exits
    return 0;
}

void start_zeroconf_thread(GUI *gui) {
    zeroconf_id = sceKernelCreateThread("zeroconf", (SceKernelThreadEntry)start_zeroconf,
                                        0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(zeroconf_id, sizeof(void*), &gui);
}

int start_cspot(SceSize _args, void *_argp) {
    GUI* gui = *((GUI**)_argp);

    CSPOT_LOG(info, "Creating player");
    auto session = std::make_unique<Session>();
    session->connectWithRandomAp();
    auto token = session->authenticate(blob);

    // Auth successful
    if (token.size() > 0) {
        // credentials ok, save for later
        file->writeFile(CREDENTIALS_FILE_NAME, blob->toJson());

        auto audioSink = std::make_shared<VitaAudioSink>();

        mercuryManager = std::make_shared<MercuryManager>(std::move(session));
        mercuryManager->startTask();

        while (!mercuryManager->isRunning) {
            sceKernelDelayThread(10000);
        }

        spircController = std::make_shared<SpircController>(mercuryManager, blob->username, audioSink);

        // Feed the shared PlayerModel; the GUI thread observes it (no casts into
        // the screen, no direct cspot coupling). get_if avoids a throwing variant
        // access if an event ever carries an unexpected payload type.
        spircController->setEventHandler([gui](CSpotEvent &event) {
            switch (event.eventType) {
                case CSpotEventType::TRACK_INFO:
                    if (auto t = std::get_if<TrackInfo>(&event.data)) {
                        gui->player.setTrack(t->name, t->album, t->artist,
                                             t->imageUrl, t->duration);
                    }
                    break;
                case CSpotEventType::PLAY_PAUSE:
                    if (auto p = std::get_if<bool>(&event.data)) {
                        gui->player.setPaused(*p);
                    }
                    break;
                case CSpotEventType::SEEK:
                    if (auto p = std::get_if<int>(&event.data)) {
                        gui->player.setPosition(*p);
                    }
                    break;
                case CSpotEventType::LOAD:
                    gui->player.setPosition(0);
                    break;
                case CSpotEventType::PLAYBACK_START:
                    gui->player.setPosition(0);
                    gui->player.setPaused(false);
                    break;
                case CSpotEventType::VOLUME:
                    if (auto p = std::get_if<int>(&event.data)) {
                        gui->player.setVolume(*p);
                    }
                    break;
                default:
                    break;
            }
        });

        // control CSpot from gui
        gui->nextCallback = []() {
            return spircController->nextSong();
        };

        gui->prevCallback = []() {
            return spircController->prevSong();
        };

        gui->activateDevice = []() {
            // if (!spircController->state->isActive()) {
            //     spircController->state->setActive(true);
            // }
            // return spircController->notify();
        };

        gui->playToggleCallback = []() {
            return spircController->playToggle();
        };

        gui->volumeCallback = [](int v) {
            return spircController->setVolume(v);
        };

        mercuryManager->reconnectedCallback = []() {
            return spircController->subscribe();
        };

        // Controls are wired: only now is it safe to let the UI call them.
        // (cspot_started=true with unassigned std::functions = bad_function_call
        // crash if the user taps a transport button during the login5 fetch.)
        gui->cspot_started = true;

        // Spotify retired the keymaster Mercury token endpoint (it now answers
        // {"code":4,"errorDescription":"Invalid request"} for stored-credential
        // sessions). Mint the Web API access token via login5 from the stored
        // credentials instead. Player controls run through spirc and don't need
        // the token, so this blocking fetch happens after they are wired.
        {
            std::string accessToken = login5_get_access_token(
                CLIENT_ID_ANDROID, DEVICE_ID, USER_AGENT, blob->username, blob->authData);
            if (!accessToken.empty()) {
                gui->api.set_token(accessToken);
            } else {
                CSPOT_LOG(error, "login5: no Web API token; in-app browsing disabled");
            }
        }

        while (gui->isRunning) {
            mercuryManager->updateQueue();
            sceKernelDelayThread(10000);
        }

        spircController->disconnect();
    }
    // Login finished: auth was rejected, or the app is shutting down. If we are
    // still running, the credentials were declined (e.g. a stale cached blob):
    // drop to the waiting screen and (re)start Zeroconf so the user can re-pair.
    gui->set_screen(gui->login_screen);
    if (gui->isRunning && zeroconf_id == 0) {
        start_zeroconf_thread(gui);
    }
    return 0;
}

void start_cspot_thread(GUI *gui) {
    cspot_id = sceKernelCreateThread("cspot", (SceKernelThreadEntry)start_cspot, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(cspot_id, sizeof(void*), &gui);
}

int print_to_menu(const char* fmt, ...);
int vprint_to_menu(const char* fmt, va_list args);

// Routes cspot/bell logs to the on-screen log view and the log file. The pinned
// bell hardcodes printf and has no output-redirect hooks, so instead of patching
// it we install our own logger (CSPOT_LOG/BELL_LOG go through bellGlobalLogger).
class MenuLogger : public bell::AbstractLogger {
    void emit(char level, const std::string& filename, int line, const char* format, va_list args) {
        const char* base = filename.c_str();
        const char* slash = strrchr(base, '/');
        if (slash) {
            base = slash + 1;
        }
        print_to_menu("%c %s:%d: ", level, base, line);
        vprint_to_menu(format, args);
        print_to_menu("\n");
    }

 public:
    void debug(std::string f, int l, std::string, const char* fmt, ...) override {
        va_list a; va_start(a, fmt); emit('D', f, l, fmt, a); va_end(a);
    }
    void error(std::string f, int l, std::string, const char* fmt, ...) override {
        va_list a; va_start(a, fmt); emit('E', f, l, fmt, a); va_end(a);
    }
    void info(std::string f, int l, std::string, const char* fmt, ...) override {
        va_list a; va_start(a, fmt); emit('I', f, l, fmt, a); va_end(a);
    }
};

int main(void) {
    // Create the data dir before opening the log file, otherwise init_logger's
    // fopen fails on a fresh install (the dir does not exist yet).
    sceIoMkdir("ux0:data/cspot", 0777);
    sceIoMkdir("ux0:data/cspot/cache", 0777);

    sceIoRemove("ux0:data/cspot/stage.txt");
    dbg_mark("01-main-start");

    init_logger();
    bell::bellGlobalLogger = std::make_shared<MenuLogger>();
    dbg_mark("02-logger");

    GUI gui;

    init_network();
    dbg_mark("03-network");

    watch_id = sceKernelCreateThread("watchdog", (SceKernelThreadEntry) watch_dog, 0x10000100, 0x4000, 0, 0, NULL);
    GUI *gui_p = &gui;
    sceKernelStartThread(watch_id, sizeof(void*), &gui_p);
    dbg_mark("04-watchdog");

    file = std::make_shared<CliFile>();
    configMan = std::make_shared<ConfigJSON>(CONFIG_FILE_NAME, file);

    if (!configMan->load()) {
        CSPOT_LOG(error, "Config error");
    }

    configMan->deviceName = DEVICE_NAME;
    configMan->format = AudioFormat_OGG_VORBIS_320;

    blob = std::make_shared<LoginBlob>();
    dbg_mark("05-config");

    gui.init();
    dbg_mark("06-gui-init-done");

    std::string authData;
    file->readFile(CREDENTIALS_FILE_NAME, authData);
    bool autoLogin = false;
    if (authData.length() > 0) {
        // Validate BEFORE LoginBlob::loadJson: that submodule code does
        // std::string(cJSON_GetStringValue(...)) with no null checks, so a
        // truncated/foreign authBlob.json (the file is shared with the old
        // CSpot install) segfaults at boot, before the first frame.
        cJSON *root = cJSON_Parse(authData.c_str());
        bool valid = false;
        if (root != NULL) {
            cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "username");
            cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "authData");
            cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "authType");
            valid = cJSON_IsString(u) && u->valuestring != NULL &&
                    cJSON_IsString(a) && a->valuestring != NULL &&
                    cJSON_IsNumber(t);
            cJSON_Delete(root);
        }
        if (!valid) {
            CSPOT_LOG(error, "authBlob.json invalid, falling back to Zeroconf");
        } else {
            blob->loadJson(authData);
            // authType 0 == AUTHENTICATION_USER_PASS is dead: Spotify removed it
            // in 2024, so a legacy cached blob always gets AUTH_DECLINED. Ignore
            // it and fall back to Zeroconf. Zeroconf blobs persist authType 1.
            if (blob->authType != 0) {
                autoLogin = true;
            } else {
                CSPOT_LOG(info, "Ignoring legacy user/pass credentials (no longer accepted)");
            }
        }
    }

    if (autoLogin) {
        start_cspot_thread(&gui);
        gui.set_screen(gui.playback_screen);
    } else {
        // Show the "waiting for Spotify Connect" screen and start advertising.
        gui.set_screen(gui.login_screen);
        start_zeroconf_thread(&gui);
    }
    dbg_mark(autoLogin ? "07-thread-autologin" : "07-thread-zeroconf");

    dbg_mark("08-pre-gui-start");
    gui.start();

    flush_logger();

    sceAppMgrReleaseBgmPort();
    term_network();

    sceKernelWaitThreadEnd(watch_id, NULL, NULL);
    sceKernelDeleteThread(watch_id);
    // cspot_id stays 0 until the user actually logs in (Zeroconf mode); only
    // join/delete it if it was started. The zeroconf thread blocks in listen()
    // and is reaped by sceKernelExitProcess below.
    if (cspot_id != 0) {
        sceKernelWaitThreadEnd(cspot_id, NULL, NULL);
        sceKernelDeleteThread(cspot_id);
    }

    flush_logger();

    sceKernelExitProcess(0);  // DO NOT REMOVE
    return 0;
}
