#include "ApResolve.h"
#include <memory>
#include <vector>
#include <string>
#include <iostream>
#include <ctype.h>
#include <cstring>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>
#include <fstream>
#include "Logger.h"
#include <cJSON.h>
#include <ConfigJSON.h>
#include <random>

ApResolve::ApResolve() {}

std::string ApResolve::getApList()
{
    // hostname lookup
    struct hostent *host = gethostbyname("apresolve.spotify.com");
    struct sockaddr_in client;

    if ((host == NULL) || (host->h_addr == NULL))
    {
        CSPOT_LOG(error, "apresolve: DNS lookup error");
        throw std::runtime_error("Resolve failed");
    }

    // Prepare socket
    bzero(&client, sizeof(client));
    client.sin_family = AF_INET;
    client.sin_port = htons(80);
    memcpy(&client.sin_addr, host->h_addr, host->h_length);

    int sockFd = socket(AF_INET, SOCK_STREAM, 0);

    // Connect to spotify's server
    if (connect(sockFd, (struct sockaddr *)&client, sizeof(client)) < 0)
    {
        close(sockFd);
        CSPOT_LOG(error, "Could not connect to apresolve");
        throw std::runtime_error("Resolve failed");
    }

    // Prepare HTTP get header. Ask explicitly for the access point list so we
    // get the modern {"accesspoint":[...]} shape as well as the legacy one.
    std::stringstream ss;
    ss << "GET /?type=accesspoint HTTP/1.1\r\n"
       << "Host: apresolve.spotify.com\r\n"
       << "Accept: application/json\r\n"
       << "Connection: close\r\n"
       << "\r\n\r\n";

    std::string request = ss.str();

    // Send the request
    if (send(sockFd, request.c_str(), request.length(), 0) != (int)request.length())
    {
        CSPOT_LOG(error, "apresolve: can't send request");
        throw std::runtime_error("Resolve failed");
    }

    char cur;

    // skip read till json data
    while (read(sockFd, &cur, 1) > 0 && cur != '{');

    auto jsonData = std::string("{");

    // Read json structure
    while (read(sockFd, &cur, 1) > 0)
    {
        jsonData += cur;
    }

    close(sockFd);

    return jsonData;
}

// Fallback AP used when apresolve is unreachable. On Vita gethostbyname for
// apresolve.spotify.com can fail right after a reconnect tears the socket down;
// throwing there aborts the whole reconnect. This known-good AP lets the session
// (re)connect directly instead of giving up.
#define AP_FALLBACK "ap-gew1.spotify.com:4070"

std::string ApResolve::fetchFirstApAddress()
{
    if (configMan->apOverride != "")
    {
        return configMan->apOverride;
    }

    try
    {
        // Fetch json body
        auto jsonData = getApList();

        auto root = cJSON_Parse(jsonData.c_str());
        if (root == NULL)
        {
            throw std::runtime_error("invalid JSON response");
        }

        // Spotify renamed the key from "ap_list" to "accesspoint"; accept both.
        auto apList = cJSON_GetObjectItemCaseSensitive(root, "ap_list");
        if (!cJSON_IsArray(apList))
        {
            apList = cJSON_GetObjectItemCaseSensitive(root, "accesspoint");
        }

        if (!cJSON_IsArray(apList) || cJSON_GetArraySize(apList) == 0)
        {
            cJSON_Delete(root);
            throw std::runtime_error("no access point in response");
        }

        auto firstAp = cJSON_GetArrayItem(apList, 0);
        if (!cJSON_IsString(firstAp) || firstAp->valuestring == NULL)
        {
            cJSON_Delete(root);
            throw std::runtime_error("malformed access point entry");
        }
        auto data = std::string(firstAp->valuestring);
        cJSON_Delete(root);
        return data;
    }
    catch (const std::exception &e)
    {
        CSPOT_LOG(error, "apresolve failed (%s), using %s", e.what(), AP_FALLBACK);
        return AP_FALLBACK;
    }
}
