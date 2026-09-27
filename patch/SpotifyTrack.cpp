#include "SpotifyTrack.h"
#include "unistd.h"
#include "MercuryManager.h"
#include <cassert>
#include "CspotAssert.h"
#include "Logger.h"
#include "ConfigJSON.h"

SpotifyTrack::SpotifyTrack(std::shared_ptr<MercuryManager> manager, std::shared_ptr<TrackReference> trackReference, uint32_t position_ms, bool isPaused)
{
    this->manager = manager;
    this->fileId = std::vector<uint8_t>();
    episodeInfo = {};
    trackInfo = {};

    mercuryCallback trackResponseLambda = [=](std::unique_ptr<MercuryResponse> res) {
        this->trackInformationCallback(std::move(res), position_ms, isPaused);
    };

    mercuryCallback episodeResponseLambda = [=](std::unique_ptr<MercuryResponse> res) {
        this->episodeInformationCallback(std::move(res), position_ms, isPaused);
    };

    if (trackReference->isEpisode)
    {
        this->reqSeqNum = this->manager->execute(MercuryType::GET, "hm://metadata/3/episode/" + bytesToHexString(trackReference->gid), episodeResponseLambda);
    }
    else
    {
        this->reqSeqNum = this->manager->execute(MercuryType::GET, "hm://metadata/3/track/" + bytesToHexString(trackReference->gid), trackResponseLambda);
    }
}

SpotifyTrack::~SpotifyTrack()
{
    // Both wait for a callback of this track running on the cspot thread,
    // and only drop what this track registered.
    this->manager->unregisterMercuryCallback(this->reqSeqNum);
    this->manager->freeAudioKeyCallback(this);
    pb_release(Track_fields, &this->trackInfo);
    pb_release(Episode_fields, &this->episodeInfo);
}

// Index of the file to stream: the configured bitrate first, then the other
// Vorbis files from the highest down. Some tracks carry only a subset, and an
// empty fileId used to stall playback with no error.
static int pickAudioFile(const AudioFile *files, pb_size_t count, AudioFormat want)
{
    const AudioFormat order[] = {want, AudioFormat_OGG_VORBIS_320,
                                 AudioFormat_OGG_VORBIS_160, AudioFormat_OGG_VORBIS_96};
    for (AudioFormat f : order)
    {
        for (int x = 0; x < count; x++)
        {
            if (files[x].format == f) return x;
        }
    }
    return -1;
}

bool SpotifyTrack::countryListContains(char *countryList, char *country)
{
    uint16_t countryList_length = strlen(countryList);
    for (int x = 0; x < countryList_length; x += 2)
    {
        if (countryList[x] == country[0] && countryList[x + 1] == country[1])
        {
            return true;
        }
    }
    return false;
}

bool SpotifyTrack::canPlayTrack(int altIndex)
{
    if(altIndex < 0)
    {
        for (int x = 0; x < trackInfo.restriction_count; x++)
        {
            if (trackInfo.restriction[x].countries_allowed != nullptr)
            {
                return countryListContains(trackInfo.restriction[x].countries_allowed, manager->countryCode);
            }

            if (trackInfo.restriction[x].countries_forbidden != nullptr)
            {
                return !countryListContains(trackInfo.restriction[x].countries_forbidden, manager->countryCode);
            }
        }
    }
    else
    {
        for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count; x++)
        {
            if (trackInfo.alternative[altIndex].restriction[x].countries_allowed != nullptr)
            {
                return countryListContains(trackInfo.alternative[altIndex].restriction[x].countries_allowed, manager->countryCode);
            }

            if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden != nullptr)
            {
                return !countryListContains(trackInfo.alternative[altIndex].restriction[x].countries_forbidden, manager->countryCode);
            }
        }
    }
    return true;
}

void SpotifyTrack::trackInformationCallback(std::unique_ptr<MercuryResponse> response, uint32_t position_ms, bool isPaused)
{
    if (this->fileId.size() != 0)
        return;
    CSPOT_ASSERT(response->parts.size() > 0, "response->parts.size() must be greater than 0");

    pb_release(Track_fields, &trackInfo);
    pbDecode(trackInfo, Track_fields, response->parts[0]);

    CSPOT_LOG(info, "Track name: %s", trackInfo.name);
    CSPOT_LOG(info, "Track duration: %d", trackInfo.duration);
    CSPOT_LOG(debug, "trackInfo.restriction.size() = %d", trackInfo.restriction_count);

    int altIndex = -1;
    while (!canPlayTrack(altIndex))
    {
        altIndex++;
        CSPOT_LOG(info, "Trying alternative %d", altIndex);
    
        if(altIndex >= trackInfo.alternative_count) {
            // no alternatives for song
            CSPOT_LOG(error, "Track not playable in this country");
            return;
        }
    }

    std::vector<uint8_t> trackId;
    this->fileId = std::vector<uint8_t>();

    if(altIndex < 0)
    {
        trackId = pbArrayToVector(trackInfo.gid);
        int x = pickAudioFile(trackInfo.file, trackInfo.file_count, configMan->format);
        if (x >= 0)
        {
            this->fileId = pbArrayToVector(trackInfo.file[x].file_id);
        }
    }
    else
    {
        trackId = pbArrayToVector(trackInfo.alternative[altIndex].gid);
        const Track &alt = trackInfo.alternative[altIndex];
        int x = pickAudioFile(alt.file, alt.file_count, configMan->format);
        if (x >= 0)
        {
            this->fileId = pbArrayToVector(alt.file[x].file_id);
        }
    }
    if (this->fileId.empty())
    {
        CSPOT_LOG(error, "No Vorbis file for this track");
    }

    if (trackInfoReceived != nullptr)
    {
        auto imageId = pbArrayToVector(trackInfo.album.cover_group.image[0].file_id);
        auto gid = pbArrayToVector(trackInfo.gid);
        TrackInfo simpleTrackInfo = {
            .name = std::string(trackInfo.name),
            .album = std::string(trackInfo.album.name),
            .artist = std::string(trackInfo.artist[0].name),
            .imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId),
            .duration = trackInfo.duration,
            .gid = bytesToHexString(gid),
        };

        trackInfoReceived(simpleTrackInfo);
    }

    this->requestAudioKey(this->fileId, trackId, trackInfo.duration, position_ms, isPaused);
}

void SpotifyTrack::episodeInformationCallback(std::unique_ptr<MercuryResponse> response, uint32_t position_ms, bool isPaused)
{
    if (this->fileId.size() != 0)
        return;
    CSPOT_LOG(debug, "Got to episode");
    CSPOT_ASSERT(response->parts.size() > 0, "response->parts.size() must be greater than 0");
    pb_release(Episode_fields, &episodeInfo);
    pbDecode(episodeInfo, Episode_fields, response->parts[0]);

    CSPOT_LOG(info, "--- Episode name: %s", episodeInfo.name);

    this->fileId = std::vector<uint8_t>();

    // TODO: option to set file quality
    for (int x = 0; x < episodeInfo.audio_count; x++)
    {
        if (episodeInfo.audio[x].format == AudioFormat_OGG_VORBIS_96)
        {
            this->fileId = pbArrayToVector(episodeInfo.audio[x].file_id);
            break; // If file found stop searching
        }
    }

    if (trackInfoReceived != nullptr)
    {
        auto imageId = pbArrayToVector(episodeInfo.covers->image[0].file_id);
        TrackInfo simpleTrackInfo = {
            .name = std::string(episodeInfo.name),
            .album = "",
            .artist = "",
            .imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId),
            .duration = trackInfo.duration,

        };

        trackInfoReceived(simpleTrackInfo);
    }

    this->requestAudioKey(this->fileId, pbArrayToVector(episodeInfo.gid), episodeInfo.duration, position_ms, isPaused);
}

void SpotifyTrack::requestAudioKey(std::vector<uint8_t> fileId, std::vector<uint8_t> trackId, int32_t trackDuration, uint32_t position_ms, bool isPaused)
{
    audioKeyCallback audioKeyLambda = [=](bool success, std::vector<uint8_t> res) {
        if (success)
        {
            CSPOT_LOG(info, "Successfully got audio key!");
            auto audioKey = std::vector<uint8_t>(res.begin() + 4, res.end());
            if (this->fileId.size() > 0)
            {
                this->audioStream = std::make_unique<ChunkedAudioStream>(this->fileId, audioKey, trackDuration, this->manager, position_ms, isPaused);
                loadedTrackCallback();
            }
            else
            {
                CSPOT_LOG(error, "Error while fetching audiokey...");
            }
        }
        else
        {
            auto code = ntohs(extract<uint16_t>(res, 4));
            CSPOT_LOG(error, "Error while fetching audiokey, error code: %d", code);
        }
    };

    this->manager->requestAudioKey(trackId, fileId, audioKeyLambda, this);
}
