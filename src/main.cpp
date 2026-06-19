#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/bgapputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <vitaGL.h>

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

        // Request token for player control
        mercuryCallback responseLambda = [=](std::unique_ptr<MercuryResponse> res) {
            if (res->parts.size() == 0) {
                CSPOT_LOG(debug, "Empty response");
                return;
            }

            cJSON *root = cJSON_Parse((const char *) res->parts[0].data());
            if (root == NULL) {
                CSPOT_LOG(error, "Token response: invalid JSON");
                return;
            }
            cJSON *accessToken = cJSON_GetObjectItem(root, "accessToken");
            if (cJSON_IsString(accessToken) && accessToken->valuestring != NULL) {
                gui->api.set_token(accessToken->valuestring);
                gui->cspot_started = true;
                CSPOT_LOG(debug, "Got token");
            } else {
                CSPOT_LOG(error, "Token response missing accessToken");
            }
            cJSON_Delete(root);
        };
        mercuryManager->execute(MercuryType::GET, "hm://keymaster/token/authenticated?scope="
                            + std::string(SCOPES) +"&client_id="
                            + std::string(CLIENT_ID_ANDROID) +"&device_id=" + std::string(DEVICE_ID), responseLambda);

        // Add event handler
        spircController->setEventHandler([gui](CSpotEvent &event) {
            switch (event.eventType) {
                case CSpotEventType::TRACK_INFO: {
                    TrackInfo track = std::get<TrackInfo>(event.data);
                    ((PlaybackScreen*) gui->playback_screen)->setTrack(track.name, track.album,
                                                                            track.artist, track.imageUrl);
                    break;
                }
                case CSpotEventType::PLAY_PAUSE: {
                    ((PlaybackScreen*) gui->playback_screen)->setPause(std::get<bool>(event.data));
                    break;
                }
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

        mercuryManager->reconnectedCallback = []() {
            return spircController->subscribe();
        };

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

int main(void) {
    // Create the data dir before opening the log file, otherwise init_logger's
    // fopen fails on a fresh install (the dir does not exist yet).
    sceIoMkdir("ux0:data/cspot", 0777);
    sceIoMkdir("ux0:data/cspot/cache", 0777);

    init_logger();
    bell::setDefaultLogger();
    bell::disableColors();
    bell::function_printf = &print_to_menu;
    bell::function_vprintf = &vprint_to_menu;

    GUI gui;

    init_network();

    watch_id = sceKernelCreateThread("watchdog", (SceKernelThreadEntry) watch_dog, 0x10000100, 0x4000, 0, 0, NULL);
    GUI *gui_p = &gui;
    sceKernelStartThread(watch_id, sizeof(void*), &gui_p);

    file = std::make_shared<CliFile>();
    configMan = std::make_shared<ConfigJSON>(CONFIG_FILE_NAME, file);

    if (!configMan->load()) {
        CSPOT_LOG(error, "Config error");
    }

    configMan->deviceName = DEVICE_NAME;
    configMan->format = AudioFormat_OGG_VORBIS_320;

    blob = std::make_shared<LoginBlob>();

    gui.init();

    std::string authData;
    file->readFile(CREDENTIALS_FILE_NAME, authData);
    bool autoLogin = false;
    if (authData.length() > 0) {
        blob->loadJson(authData);
        // authType 0 == AUTHENTICATION_USER_PASS is dead: Spotify removed it in
        // 2024, so a legacy cached blob always gets AUTH_DECLINED. Ignore it and
        // fall back to the Zeroconf flow. Zeroconf blobs persist authType 1.
        if (blob->authType != 0) {
            autoLogin = true;
        } else {
            CSPOT_LOG(info, "Ignoring legacy user/pass credentials (no longer accepted)");
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
