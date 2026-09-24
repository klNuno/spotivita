#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/bgapputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <curl/curl.h>

#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <utility>
#include <vector>

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
#include "DevKit.h"
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

// Player commands from the GUI (play/pause, next, prev, volume). SpircController
// is not thread-safe and the cspot thread drives it through updateQueue, so the
// GUI queues closures here and the cspot thread runs them between updates.
static std::mutex cspot_cmd_mutex;
static std::deque<std::function<void()>> cspot_cmds;

static void queue_cspot(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(cspot_cmd_mutex);
    if (cspot_cmds.size() < 32) {
        cspot_cmds.push_back(std::move(fn));
    }
}

static void run_cspot_cmds() {
    std::deque<std::function<void()>> cmds;
    {
        std::lock_guard<std::mutex> g(cspot_cmd_mutex);
        cmds.swap(cspot_cmds);
    }
    for (auto &c : cmds) {
        c();
    }
}

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
        // No pending event leaves appEvent untouched: only act on a real one.
        memset(&appEvent, 0, sizeof(appEvent));
        if (sceAppMgrReceiveEvent(&appEvent) < 0) {
            appEvent.event = 0;
        }
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
    // 128 KB: the Zeroconf handshake runs a DH exchange and AES on this stack.
    zeroconf_id = sceKernelCreateThread("zeroconf", (SceKernelThreadEntry)start_zeroconf,
                                        0x10000100, 0x20000, 0, 0, NULL);
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

        // Control cspot from the GUI, through the command queue.
        gui->nextCallback = []() {
            queue_cspot([] { spircController->nextSong(); });
        };
        gui->prevCallback = []() {
            queue_cspot([] { spircController->prevSong(); });
        };
        gui->playToggleCallback = []() {
            queue_cspot([] { spircController->playToggle(); });
        };
        gui->volumeCallback = [](int v) {
            queue_cspot([v] { spircController->setVolume(v); });
        };
        gui->playTracksCallback = [](const std::vector<std::string> &uris,
                                     const std::string &context, uint32_t index) {
            queue_cspot([uris, context, index] {
                spircController->playTracks(uris, context, index);
            });
        };
        gui->seekCallback = [](int ms) {
            queue_cspot([ms] { spircController->seek(static_cast<uint32_t>(ms)); });
        };
        gui->shuffleCallback = [](bool on) {
            queue_cspot([on] { spircController->setShuffle(on); });
        };
        gui->repeatCallback = [](int mode) {
            queue_cspot([mode] { spircController->setRepeat(mode); });
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
            // The net worker re-mints the token through this when it expires
            // (login5 tokens last one hour).
            std::shared_ptr<LoginBlob> creds = blob;
            gui->api.set_user(blob->username);  // needed for the spclient rootlist URL
            gui->api.set_refresher([creds](int *expiresIn) {
                return login5_get_access_token(CLIENT_ID_ANDROID, DEVICE_ID, USER_AGENT,
                                               creds->username, creds->authData, expiresIn);
            });
            int expiresIn = 0;
            std::string accessToken = login5_get_access_token(
                CLIENT_ID_ANDROID, DEVICE_ID, USER_AGENT, blob->username, blob->authData, &expiresIn);
            if (!accessToken.empty()) {
                gui->api.set_token(accessToken, expiresIn);
            } else {
                CSPOT_LOG(error, "login5: no Web API token; in-app browsing disabled");
            }
        }

        while (gui->isRunning) {
            // updateQueue dispatches Mercury packets to spirc/track callbacks,
            // some of which send Mercury packets back. A send on a blipped AP
            // link throws std::runtime_error; with no guard here it unwinds out
            // of the cspot thread -> std::terminate -> abort (the crash seen in
            // the coredump: _kill_r / __verbose_terminate_handler on "cspot").
            // Heavy spclient traffic makes the blip likelier. Contain it: the
            // separate recv (runTask) thread owns reconnection and rebuilds the
            // session on its own, so here we just drop the failed dispatch.
            try {
                run_cspot_cmds();
                mercuryManager->updateQueue();
            } catch (const std::exception& e) {
                CSPOT_LOG(error, "updateQueue exception contained: %s", e.what());
            } catch (...) {
                CSPOT_LOG(error, "updateQueue exception contained (unknown)");
            }
            sceKernelDelayThread(10000);
        }

        spircController->disconnect();
    }
    // Login finished: auth was rejected, or the app is shutting down. If we are
    // still running, the credentials were declined (e.g. a stale cached blob):
    // drop to the waiting screen and (re)start Zeroconf so the user can re-pair.
    gui->set_screen(gui->login_screen);
    if (gui->isRunning && zeroconf_id == 0 && !loopback_mode()) {
        start_zeroconf_thread(gui);
    }
    return 0;
}

void start_cspot_thread(GUI *gui) {
    // 256 KB: session auth (DH, Shannon), login5 over curl/OpenSSL and the
    // hashcash solver all run on this thread; 64 KB was one deep call away
    // from a silent stack overflow.
    cspot_id = sceKernelCreateThread("cspot", (SceKernelThreadEntry)start_cspot, 0x10000100, 0x40000, 0, 0, NULL);
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
        // One call per line: three separate calls interleaved between threads.
        char msg[768];
        vsnprintf(msg, sizeof(msg), format, args);
        print_to_menu("%c %s:%d: %s\n", level, base, line, msg);
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
    // MUST run before any thread touches curl: curl_easy_init's lazy global
    // init is not thread-safe, and at boot the login5 fetch (cspot thread) and
    // the first playlist fetch (GUI thread) can race it, corrupting libcurl /
    // OpenSSL global state.
    curl_global_init(CURL_GLOBAL_ALL);
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
    DevKit::start(&gui);
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
        if (loopback_mode()) {
            CSPOT_LOG(info, "loopback mode: Zeroconf not started");
        } else {
            start_zeroconf_thread(&gui);
        }
    }
    dbg_mark(autoLogin ? "07-thread-autologin" : "07-thread-zeroconf");

    dbg_mark("08-pre-gui-start");
    gui.start();  // returns when the watchdog sets isRunning=false (quit/logout)

    // Do NOT gracefully join the worker threads here. On shutdown the cspot
    // thread runs spircController->disconnect() (a blocking network write); if
    // we tore the net stack down first (term_network) or the AP link is slow,
    // that call hangs forever, so sceKernelWaitThreadEnd(cspot_id) never
    // returns. The process then never reaches exitProcess, keeps holding the
    // GXM display, and the LiveArea shell can't reclaim it -> the console menu
    // degrades and freezes after "closing" the app. sceKernelExitProcess
    // terminates every thread and releases GXM, the audio port, and the net
    // stack, so just flush the log and exit immediately.
    flush_logger();
    sceKernelExitProcess(0);  // DO NOT REMOVE: kernel reaps all threads/handles
    return 0;
}
