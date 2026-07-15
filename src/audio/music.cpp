#include <audio/music.h>
#include <audio/source.h>
#include <resources.h>
#include "logging.h"


#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wshadow-compatible-local"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif//__GNUC__
#include "stb/stb_vorbis.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif//__GNUC__


namespace sp {
namespace audio {

bool Music::open(const string& resource_name, bool loop)
{
    auto stream = getResourceStream(resource_name);
    if (!stream)
    {
        LOG(Error, "Failed to open", resource_name, "to play as music");
        return false;
    }
    stop();
    if (vorbis)
        stb_vorbis_close(reinterpret_cast<stb_vorbis*>(vorbis));

    file_data.resize(stream->getSize());
    stream->read(file_data.data(), file_data.size());
    int error = 0;
    vorbis = stb_vorbis_open_memory(file_data.data(), static_cast<int>(file_data.size()), &error, nullptr);
    if (!vorbis)
    {
        LOG(Error, "Failed to read music file ", resource_name, " error: ", error);
        return false;
    }

    auto info = stb_vorbis_get_info(reinterpret_cast<stb_vorbis*>(vorbis));
    sample_rate = info.sample_rate;
    length = stb_vorbis_stream_length_in_samples(reinterpret_cast<stb_vorbis*>(vorbis));
    start();
    return true;
}

void Music::setVolume(float _volume)
{
    volume = _volume / 100.0f;
}

string Music::getTagsDisplayName(const string& resource_name)
{
    auto stream = getResourceStream(resource_name);
    if (!stream)
        return resource_name.substr(resource_name.rfind("/") + 1, resource_name.rfind("."));

    std::vector<uint8_t> file_data;
    file_data.resize(stream->getSize());
    stream->read(file_data.data(), file_data.size());

    int error = 0;
    auto* v = stb_vorbis_open_memory(file_data.data(), static_cast<int>(file_data.size()), &error, nullptr);
    if (!v)
        return resource_name.substr(resource_name.rfind("/") + 1, resource_name.rfind("."));

    auto comment = stb_vorbis_get_comment(v);

    string artist;
    string title;
    for (int i = 0; i < comment.comment_list_length; i++)
    {
        string line(comment.comment_list[i]);
        int eq = line.find("=");
        if (eq > 0)
        {
            string key = line.substr(0, eq).upper();
            string value = line.substr(eq + 1);
            if (key == "ARTIST")
                artist = value;
            else if (key == "TITLE")
                title = value;
        }
    }

    stb_vorbis_close(v);

    if (!artist.empty() && !title.empty())
        return artist + " - " + title;
    if (!title.empty())
        return title;

    return resource_name.substr(resource_name.rfind("/") + 1, resource_name.rfind("."));
}

void Music::onMixSamples(int16_t* stream, int sample_count)
{
    static std::vector<int16_t> buffer;
    buffer.resize(sample_count);
    int vorbis_samples = stb_vorbis_get_samples_short_interleaved(reinterpret_cast<stb_vorbis*>(vorbis), 2, buffer.data(), sample_count) * 2;
    if (vorbis_samples == 0)
    {
        if (loop)
            stb_vorbis_seek_frame(reinterpret_cast<stb_vorbis*>(vorbis), 0);
        else
            stop();
    }
    //TODO: Handle sample_rate != 44100
    for(int idx=0; idx<vorbis_samples; idx++)
        stream[idx] = std::clamp(int(stream[idx] + buffer[idx] * volume), int(std::numeric_limits<int16_t>::min()), int(std::numeric_limits<int16_t>::max()));
}

}//namespace audio
}//namespace sp
