
#include "PlainConnection.h"
#include <cstring>
#include <netinet/tcp.h>
#include <errno.h>
#include <atomic>
#include <stdexcept>
#include "Logger.h"

// Vita changes to cspot's PlainConnection: with the network gone, upstream
// walked an uninitialised addrinfo list after a failed getaddrinfo (crash on
// every reconnect attempt and at boot), spun on a closed socket (recv returns
// 0 at once, errno kept a stale EAGAIN), and added a -1 read count to the
// unsigned index. Every failure now throws, which MercuryManager::reconnect and
// start_cspot turn into a retry after a pause.

// A packet bigger than this is a desynced stream, not a real packet.
#define MAX_PACKET_SIZE (4 * 1024 * 1024)
// Without a timeout handler (handshake, login), give up after this many
// socket timeouts of 3 s.
#define MAX_BARE_TIMEOUTS 5

// Spotivita devkit ("netdrop write"): the next write fails, as a send on a
// dead Wi-Fi link does.
static std::atomic<bool> failNextWrite{false};
void spotivita_fail_next_write() { failNextWrite = true; }

PlainConnection::PlainConnection()
{
	this->apSock = -1;
};

PlainConnection::~PlainConnection()
{
    closeSocket();
};

void PlainConnection::connectToAp(std::string apAddress)
{
    struct addrinfo h, *airoot = NULL, *ai;
    std::string hostname = apAddress.substr(0, apAddress.find(":"));
    std::string portStr = apAddress.substr(apAddress.find(":") + 1, apAddress.size());
    memset(&h, 0, sizeof(h));
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    h.ai_protocol = IPPROTO_IP;

    // Lookup host
    if (getaddrinfo(hostname.c_str(), portStr.c_str(), &h, &airoot) != 0 || airoot == NULL)
    {
        CSPOT_LOG(error, "getaddrinfo failed for %s", hostname.c_str());
        throw std::runtime_error("Can't resolve spotify servers");
    }

    // find the right ai, connect to server
    for (ai = airoot; ai; ai = ai->ai_next)
    {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
            continue;

        this->apSock = socket(ai->ai_family,
                              ai->ai_socktype, ai->ai_protocol);
        if (this->apSock < 0)
            continue;

        if (connect(this->apSock,
                    (struct sockaddr *)ai->ai_addr,
                    ai->ai_addrlen) != -1)
        {
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(this->apSock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
            setsockopt(this->apSock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);

            int flag = 1;
            setsockopt(this->apSock,  /* socket affected */
                       IPPROTO_TCP,   /* set option at TCP level */
                       TCP_NODELAY,   /* name of option */
                       (char *)&flag, /* the cast is historical cruft */
                       sizeof(int));  /* length of option value */
            break;
        }

        close(this->apSock);
        this->apSock = -1;
    }

    freeaddrinfo(airoot);
    if (this->apSock < 0)
    {
        throw std::runtime_error("Can't connect to spotify servers");
    }
    CSPOT_LOG(debug, "Connected to spotify server");
}

std::vector<uint8_t> PlainConnection::recvPacket()
{
    // Read packet size
    auto sizeData = readBlock(4);
    uint32_t packetSize = ntohl(extract<uint32_t>(sizeData, 0));
    if (packetSize < 4 || packetSize > MAX_PACKET_SIZE)
    {
        throw std::runtime_error("Bad packet size");
    }

    // Read actual data
    auto data = readBlock(packetSize - 4);
    sizeData.insert(sizeData.end(), data.begin(), data.end());

    return sizeData;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(const std::vector<uint8_t> &prefix, const std::vector<uint8_t> &data)
{
    // Calculate full packet length
    uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);

    // Packet structure [PREFIX] + [SIZE] +  [DATA]
    auto sizeRaw = pack<uint32_t>(htonl(actualSize));
    sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
    sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

    // Actually write it to the server
    writeBlock(sizeRaw);

    return sizeRaw;
}

// A socket timeout (3 s): true when the caller should give up.
static bool timedOut(const timeoutCallback &handler, int &bareTimeouts)
{
    if (handler)
    {
        return handler();
    }
    return ++bareTimeouts >= MAX_BARE_TIMEOUTS;
}

std::vector<uint8_t> PlainConnection::readBlock(size_t size)
{
    std::vector<uint8_t> buf(size);
    size_t idx = 0;
    int retries = 0;
    int bareTimeouts = 0;

    while (idx < size)
    {
        if (this->apSock < 0)
        {
            throw std::runtime_error("Reconnection required");
        }
        ssize_t n = recv(this->apSock, &buf[idx], size - idx, 0);
        if (n > 0)
        {
            idx += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
        {
            CSPOT_LOG(error, "Connection closed by the server");
            throw std::runtime_error("Reconnection required");
        }
        switch (errno)
        {
        case EAGAIN:
        case ETIMEDOUT:
            if (timedOut(timeoutHandler, bareTimeouts))
            {
                CSPOT_LOG(error, "Connection lost, will need to reconnect...");
                throw std::runtime_error("Reconnection required");
            }
            break;
        case EINTR:
            break;
        default:
            CSPOT_LOG(error, "recv failed, errno %d", errno);
            if (retries++ > 4) throw std::runtime_error("Error in read");
            usleep(100 * 1000);
        }
    }
    return buf;
}

size_t PlainConnection::writeBlock(const std::vector<uint8_t> &data)
{
    size_t idx = 0;
    int retries = 0;
    int bareTimeouts = 0;

    if (failNextWrite.exchange(false))
    {
        throw std::runtime_error("Error in write (devkit)");
    }
    while (idx < data.size())
    {
        if (this->apSock < 0)
        {
            throw std::runtime_error("Reconnection required");
        }
        ssize_t n = send(this->apSock, &data[idx], data.size() - idx < 64 ? data.size() - idx : 64, 0);
        if (n > 0)
        {
            idx += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
        {
            usleep(100 * 1000);   // nothing sent and no error: do not spin
        }
        switch (n == 0 ? EAGAIN : errno)
        {
        case EAGAIN:
        case ETIMEDOUT:
            if (timedOut(timeoutHandler, bareTimeouts))
            {
                throw std::runtime_error("Reconnection required");
            }
            break;
        case EINTR:
            break;
        default:
            CSPOT_LOG(error, "send failed, errno %d", errno);
            if (retries++ > 4) throw std::runtime_error("Error in write");
            usleep(100 * 1000);
        }
    }

    return data.size();
}

void PlainConnection::closeSocket()
{
	if (this->apSock < 0) return;

	CSPOT_LOG(info, "Closing socket...");
	shutdown(this->apSock, SHUT_RDWR);
	close(this->apSock);
	this->apSock = -1;
}
