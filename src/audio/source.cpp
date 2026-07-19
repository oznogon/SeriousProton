#include "audio/source.h"
#include "logging.h"

#include <SDL3/SDL.h>
#include <mutex>
#include <array>
#include <string.h>

namespace sp {
namespace audio {


static std::recursive_mutex source_list_mutex;
static Source* source_list_start = nullptr;

static SDL_AudioStream* audio_stream;

static void SDLCALL AudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
    if (additional_amount <= 0)
        return;

    uint8_t* data = (uint8_t*)SDL_malloc(additional_amount);
    if (!data)
        return;

    memset(data, 0, additional_amount);
    Source::onAudioCallback(reinterpret_cast<int16_t*>(data), additional_amount / 2);
    SDL_PutAudioStreamData(stream, data, additional_amount);
    SDL_free(data);
}

Source::~Source()
{
    stop();
}

void Source::start()
{
    std::lock_guard<std::recursive_mutex> guard(source_list_mutex);
    if (active)
        return;

    active = true;
    next = source_list_start;
    if (next)
        next->previous = this;
    previous = nullptr;
    source_list_start = this;
}

bool Source::isPlaying()
{
    return active;
}

void Source::stop()
{
    std::lock_guard<std::recursive_mutex> guard(source_list_mutex);
    if (!active)
        return;

    active = false;
    if (source_list_start == this)
    {
        source_list_start = next;
        if (source_list_start)
            source_list_start->previous = nullptr;
    }
    else
    {
        previous->next = next;
    }
    if (next)
        next->previous = previous;
}

void Source::startAudioSystem()
{
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, nullptr);
    if (!audio_stream)
    {
        LOG(Error, "Failed to open audio device: ", SDL_GetError());
    } else {
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    }
}

void Source::stopAudioSystem()
{
    if (audio_stream)
    {
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(audio_stream);
        if (dev)
            SDL_PauseAudioDevice(dev);
    }
}

void Source::onAudioCallback(int16_t* stream, int sample_count)
{
    memset(stream, 0, sample_count * sizeof(int16_t));
    for(Source* source = source_list_start; source; source = source->next)
        source->onMixSamples(stream, sample_count);
}

}//namespace audio
}//namespace sp
