#include "ShannonConnection.h"
#include <stdexcept>
#include <mutex>
#include <sys/socket.h>
#include "Logger.h"

ShannonConnection::ShannonConnection()
{
}

ShannonConnection::~ShannonConnection()
{
}

void ShannonConnection::wrapConnection(std::shared_ptr<PlainConnection> conn, std::vector<uint8_t> &sendKey, std::vector<uint8_t> &recvKey)
{
    this->conn = conn;

    this->sendCipher = std::make_unique<Shannon>();
    this->recvCipher = std::make_unique<Shannon>();

    // Set keys
    this->sendCipher->key(sendKey);
    this->recvCipher->key(recvKey);

    // Set initial nonce
    this->sendCipher->nonce(pack<uint32_t>(htonl(0)));
    this->recvCipher->nonce(pack<uint32_t>(htonl(0)));
}

// VITA PATCH: a failed write used to throw with writeMutex still locked. The
// next ping reply then blocked the recv thread for good (Spotify silent until
// the app restarts), and a throw on the player thread went through libvorbis
// and aborted. Now a failed write shuts the socket down and returns: the recv
// thread sees the closed link and reconnects, and the reconnect fails pending
// chunks, which their readers ask for again.
void ShannonConnection::sendPacket(uint8_t cmd, std::vector<uint8_t> &data)
{
    std::lock_guard<WrappedMutex> guard(this->writeMutex);
    auto rawPacket = this->cipherPacket(cmd, data);

    // Shannon encrypt the packet and write it to sock
    this->sendCipher->encrypt(rawPacket);

    // Generate mac
    std::vector<uint8_t> mac(MAC_SIZE);
    this->sendCipher->finish(mac);

    // Update the nonce
    this->sendNonce += 1;
    this->sendCipher->nonce(pack<uint32_t>(htonl(this->sendNonce)));

    try
    {
        this->conn->writeBlock(rawPacket);
        this->conn->writeBlock(mac);
    }
    catch (const std::exception &e)
    {
        CSPOT_LOG(error, "Shannon write failed (%s), dropping the link", e.what());
        if (this->conn->apSock >= 0)
        {
            shutdown(this->conn->apSock, SHUT_RDWR);
        }
    }
}

std::unique_ptr<Packet> ShannonConnection::recvPacket()
{
    std::lock_guard<WrappedMutex> guard(this->readMutex);
    // Receive 3 bytes, cmd + int16 size
    auto data = this->conn->readBlock(3);
    this->recvCipher->decrypt(data);

    auto packetData = std::vector<uint8_t>();

    auto readSize = ntohs(extract<uint16_t>(data, 1));

    // Read and decode if the packet has an actual body
    if (readSize > 0)
    {
        packetData = this->conn->readBlock(readSize);
        this->recvCipher->decrypt(packetData);
    }

    // Read mac
    auto mac = this->conn->readBlock(MAC_SIZE);

    // Generate mac
    std::vector<uint8_t> mac2(MAC_SIZE);
    this->recvCipher->finish(mac2);

    if (mac != mac2)
    {
        // VITA PATCH: a MAC mismatch means the stream cipher is desynced and
        // every subsequent read is garbage. The upstream code only logged and
        // kept reading, which spun this thread in a tight error loop (and on
        // the Vita starved the GUI thread mid-GPU-frame, wedging the whole
        // console). Throw instead: MercuryManager already catches
        // runtime_error from recvPacket and runs its reconnection path, which
        // rebuilds the session with fresh Shannon keys and resubscribes.
        CSPOT_LOG(error, "Shannon read: Mac doesn't match, resetting connection");
        throw std::runtime_error("shannon mac mismatch");
    }

    // Update the nonce
    this->recvNonce += 1;
    this->recvCipher->nonce(pack<uint32_t>(htonl(this->recvNonce)));

    // data[0] == cmd
    return std::make_unique<Packet>(data[0], packetData);
}

std::vector<uint8_t> ShannonConnection::cipherPacket(uint8_t cmd, std::vector<uint8_t> &data)
{
    // Generate packet structure, [Command] [Size] [Raw data]
    auto sizeRaw = pack<uint16_t>(htons(uint16_t(data.size())));

    sizeRaw.insert(sizeRaw.begin(), cmd);
    sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

    return sizeRaw;
}
