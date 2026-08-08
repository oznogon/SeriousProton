#pragma once

#include <unordered_map>
#include "stringImproved.h"
#include "ecs/system.h"
#include "P.h"

#ifdef WIN32
#include "dynamicLibrary.h"
#endif

#ifdef STEAMSDK
// Initializes the Steam API. Exits the process if Steam relaunches the app.
void initSteamAPI();
#endif


class Engine;
union SDL_Event;
extern Engine* engine;

class Engine
{
public:
    using EngineTiming = std::map<string, float>;

private:
    bool running = true;
    bool collect_engine_timing = false;

    std::unordered_map<string, P<PObject> > objectMap;
    // Elapsed scenario time, which progresses at a rate defined by game_speed.
    // This doesn't report wall-clock time. See sp::SystemStopwatch.
    float elapsed_time = 0.0f;
    // Time scale. 1.0 is normal, 0.0 is paused.
    float game_speed = 1.0f;

    EngineTiming last_engine_timing;
#ifdef WIN32
    std::unique_ptr<DynamicLibrary> exchndl;
#endif
#ifdef __EMSCRIPTEN__
    bool audio_started = false;
#endif
    std::vector<sp::ecs::System*> systems;
public:
    Engine();
    ~Engine();

    void setGameSpeed(float speed);
    float getGameSpeed();
    float getElapsedTime();
    EngineTiming getEngineTiming();

    void registerObject(string name, P<PObject> obj);
    P<PObject> getObject(string name);

    template<class T> void registerSystem() {
        systems.push_back(new T());
    }

    void runMainLoop();
    void shutdown();
    bool isRunning() { return running; }
    bool isCollectingEngineTiming() { return collect_engine_timing; }
    void setCollectEngineTiming(bool value = true) { collect_engine_timing = value; }
private:
    void handleEvent(SDL_Event& event);
};
