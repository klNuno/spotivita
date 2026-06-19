#include "ZeroconfAuthenticator.h"
#include "JSONObject.h"
#include <sstream>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "Logger.h"
#include "ConfigJSON.h"

#ifdef VITA
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// provide weak deviceId (see ConstantParameters.h)
char deviceId[] __attribute__((weak)) = "142137fd329622137a14901634264e6f332e2411";

#ifdef VITA
// ----------------------------------------------------------------------------
// Minimal mDNS responder for the PS Vita.
//
// vitasdk ships no Bonjour / dns_sd, so we advertise the Spotify Connect
// service (_spotify-connect._tcp) by hand: a background thread sends an
// unsolicited mDNS announcement (PTR + SRV + TXT + A) to 224.0.0.251:5353 every
// few seconds and re-announces whenever it sees traffic on the mDNS port. That
// is enough for the phone's Spotify app to list the Vita as a Connect device.
//
// UNVERIFIED ON-DEVICE: depends on vitasdk socket multicast support
// (IP_ADD_MEMBERSHIP / sendto to a multicast group). If discovery proves flaky,
// the /spotify_info HTTP endpoint still works by direct IP.
// ----------------------------------------------------------------------------
namespace {

void put_name(std::vector<uint8_t>& b, const std::string& name) {
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        size_t len = dot - start;
        b.push_back(static_cast<uint8_t>(len));
        for (size_t i = start; i < dot; i++) b.push_back(static_cast<uint8_t>(name[i]));
        start = dot + 1;
    }
    b.push_back(0);
}

std::vector<uint8_t> build_announcement(const std::string& instanceLabel,
                                        const std::string& host,
                                        uint16_t port,
                                        const uint8_t ip[4]) {
    const std::string svc = "_spotify-connect._tcp.local";
    const std::string inst = instanceLabel + "." + svc;

    std::vector<uint8_t> b;
    auto u16 = [&](uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); };
    auto u32 = [&](uint32_t v) {
        b.push_back((v >> 24) & 0xff); b.push_back((v >> 16) & 0xff);
        b.push_back((v >> 8) & 0xff);  b.push_back(v & 0xff);
    };

    // header: response + authoritative, 4 answers
    u16(0); u16(0x8400); u16(0); u16(4); u16(0); u16(0);

    // PTR  _spotify-connect._tcp.local -> instance
    put_name(b, svc); u16(12); u16(0x0001); u32(4500);
    { std::vector<uint8_t> rd; put_name(rd, inst);
      u16(static_cast<uint16_t>(rd.size())); b.insert(b.end(), rd.begin(), rd.end()); }

    // SRV  instance -> host:port (cache-flush bit set)
    put_name(b, inst); u16(33); u16(0x8001); u32(120);
    { std::vector<uint8_t> rd;
      auto p16 = [&](uint16_t v) { rd.push_back(v >> 8); rd.push_back(v & 0xff); };
      p16(0); p16(0); p16(port); put_name(rd, host);
      u16(static_cast<uint16_t>(rd.size())); b.insert(b.end(), rd.begin(), rd.end()); }

    // TXT  instance
    put_name(b, inst); u16(16); u16(0x8001); u32(4500);
    { std::vector<uint8_t> rd;
      auto add = [&](const std::string& s) {
          rd.push_back(static_cast<uint8_t>(s.size()));
          rd.insert(rd.end(), s.begin(), s.end());
      };
      add("VERSION=1.0"); add("CPath=/spotify_info"); add("Stack=SP");
      u16(static_cast<uint16_t>(rd.size())); b.insert(b.end(), rd.begin(), rd.end()); }

    // A  host -> ip
    put_name(b, host); u16(1); u16(0x8001); u32(120);
    u16(4);
    b.push_back(ip[0]); b.push_back(ip[1]); b.push_back(ip[2]); b.push_back(ip[3]);

    return b;
}

bool get_local_ip(uint8_t out[4]) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = inet_addr("8.8.8.8");
    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof dst) == 0) {
        struct sockaddr_in local; socklen_t len = sizeof local;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            memcpy(out, &local.sin_addr.s_addr, 4);
            ok = true;
        }
    }
    close(s);
    return ok;
}

void mdns_thread(uint16_t port, std::string instanceLabel) {
    uint8_t ip[4];
    // Wait for the network to come up.
    while (!get_local_ip(ip)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    const std::string host = "cspot-vita.local";

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        CSPOT_LOG(error, "mdns: socket() failed");
        return;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5353);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);

    // Join the mDNS multicast group (best-effort; sending still works without).
    struct ip_mreq mreq; memset(&mreq, 0, sizeof mreq);
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);

    struct sockaddr_in mdst; memset(&mdst, 0, sizeof mdst);
    mdst.sin_family = AF_INET;
    mdst.sin_port = htons(5353);
    mdst.sin_addr.s_addr = inet_addr("224.0.0.251");

    auto pkt = build_announcement(instanceLabel, host, port, ip);
    CSPOT_LOG(info, "mdns: advertising _spotify-connect._tcp on port %d", (int)port);

    fd_set rf;
    struct timeval tv;
    uint8_t buf[1500];
    while (true) {
        sendto(s, pkt.data(), pkt.size(), 0,
               reinterpret_cast<sockaddr*>(&mdst), sizeof mdst);

        FD_ZERO(&rf); FD_SET(s, &rf);
        tv.tv_sec = 5; tv.tv_usec = 0;
        int r = select(s + 1, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(s, &rf)) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            int n = recvfrom(s, buf, sizeof buf, 0,
                             reinterpret_cast<sockaddr*>(&from), &fl);
            // Any query on the mDNS port -> re-announce immediately.
            if (n > 0) {
                sendto(s, pkt.data(), pkt.size(), 0,
                       reinterpret_cast<sockaddr*>(&mdst), sizeof mdst);
            }
        }
    }
}

}  // namespace
#endif  // VITA

ZeroconfAuthenticator::ZeroconfAuthenticator(authCallback callback, std::shared_ptr<bell::BaseHTTPServer> httpServer) {
    this->gotBlobCallback = callback;
    srand((unsigned int)time(NULL));

    this->crypto = std::make_unique<Crypto>();
    this->crypto->dhInit();
    this->server = httpServer;
}

void ZeroconfAuthenticator::registerHandlers() {
    // Make it discoverable for spoti clients
    registerZeroconf();
    auto getInfoHandler = [this](bell::HTTPRequest& request) {
        CSPOT_LOG(info, "Got request for info");
        bell::HTTPResponse response = {
            .connectionFd = request.connection,
            .status = 200,
            .body = this->buildJsonInfo(),
            .contentType = "application/json",
        };
        server->respond(response);
    };

    auto addUserHandler = [this](bell::HTTPRequest& request) {
        BELL_LOG(info, "http", "Got request for adding user");
        bell::JSONObject obj;
        obj["status"] = 101;
        obj["spotifyError"] = 0;
        obj["statusString"] = "ERROR-OK";

        bell::HTTPResponse response = {
            .connectionFd = request.connection,
            .status = 200,
            .body = obj.toString(),
            .contentType = "application/json",
        };
        server->respond(response);

        auto correctBlob = this->getParameterFromUrlEncoded(request.body, "blob");
        this->handleAddUser(request.queryParams);
    };

    BELL_LOG(info, "cspot", "Zeroconf registering handlers");
    this->server->registerHandler(bell::RequestType::GET, "/spotify_info", getInfoHandler);
    this->server->registerHandler(bell::RequestType::POST, "/spotify_info", addUserHandler);
}

void ZeroconfAuthenticator::registerZeroconf()
{
    const char* service = "_spotify-connect._tcp";
    (void)service;

#ifdef ESP_PLATFORM
    mdns_txt_item_t serviceTxtData[3] = {
        {"VERSION", "1.0"},
        {"CPath", "/spotify_info"},
        {"Stack", "SP"} };
    mdns_service_add("cspot", "_spotify-connect", "_tcp", this->server->serverPort, serviceTxtData, 3);

#elif defined(VITA)
    // Vita: hand-rolled mDNS responder (vitasdk has no Bonjour/dns_sd).
    std::thread(mdns_thread,
                static_cast<uint16_t>(this->server->serverPort),
                configMan->deviceName).detach();
#else
    // DNSServiceRef ref = NULL;
    // TXTRecordRef txtRecord;
    // TXTRecordCreate(&txtRecord, 0, NULL);
    // TXTRecordSetValue(&txtRecord, "VERSION", 3, "1.0");
    // TXTRecordSetValue(&txtRecord, "CPath", 13, "/spotify_info");
    // TXTRecordSetValue(&txtRecord, "Stack", 2, "SP");
    // DNSServiceRegister(&ref, 0, 0, (char*)informationString, service, NULL, NULL, htons(this->server->serverPort), TXTRecordGetLength(&txtRecord), TXTRecordGetBytesPtr(&txtRecord), NULL, NULL);
    // TXTRecordDeallocate(&txtRecord);
#endif
}

std::string ZeroconfAuthenticator::getParameterFromUrlEncoded(std::string data, std::string param)
{
    auto startStr = data.substr(data.find("&" + param + "=") + param.size() + 2, data.size());
    return urlDecode(startStr.substr(0, startStr.find("&")));
}

void ZeroconfAuthenticator::handleAddUser(std::map<std::string, std::string>& queryData)
{
    // Get all urlencoded params
    auto username = queryData["userName"];
    auto blobString = queryData["blob"];
    auto clientKeyString = queryData["clientKey"];
    auto deviceName = queryData["deviceName"];

    // client key and bytes are urlencoded
    auto clientKeyBytes = crypto->base64Decode(clientKeyString);
    auto blobBytes = crypto->base64Decode(blobString);

    // Generated secret based on earlier generated DH
    auto secretKey = crypto->dhCalculateShared(clientKeyBytes);

    auto loginBlob = std::make_shared<LoginBlob>();

    std::string deviceIdStr = deviceId;

    loginBlob->loadZeroconf(blobBytes, secretKey, deviceIdStr, username);

    gotBlobCallback(loginBlob);
}

std::string ZeroconfAuthenticator::buildJsonInfo()
{
    // Encode publicKey into base64
    auto encodedKey = crypto->base64Encode(crypto->publicKey);

    bell::JSONObject obj;
    obj["status"] = 101;
    obj["statusString"] = "OK";
    obj["version"] = protocolVersion;
    obj["spotifyError"] = 0;
    obj["libraryVersion"] = swVersion;
    obj["accountReq"] = "PREMIUM";
    obj["brandDisplayName"] = brandName;
    obj["modelDisplayName"] = configMan->deviceName.c_str();
    obj["voiceSupport"] = "NO";
    obj["availability"] = "";
    obj["productID"] = 0;
    obj["tokenType"] = "default";
    obj["groupStatus"] = "NONE";
    obj["resolverVersion"] = "0";
    obj["scope"] = "streaming,client-authorization-universal";
    obj["activeUser"] = "";
    obj["deviceID"] = deviceId;
    obj["remoteName"] = configMan->deviceName.c_str();
    obj["publicKey"] = encodedKey;
    obj["deviceType"] = "SPEAKER";
    return obj.toString();
}
