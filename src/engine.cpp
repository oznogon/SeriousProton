#include "engine.h"
#include "random.h"
#include "Updatable.h"
#include "audio/source.h"
#include "io/keybinding.h"
#include "soundManager.h"
#include "windowManager.h"
#include "multiplayer_server.h"
#include "ecs/entity.h"
#include "systems/collision.h"

#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <SDL3/SDL.h>

#if defined(__GNUG__) || defined(__clang__)
#include <cxxabi.h>
#endif

static string demangle(const char* mangled_name)
{
    static std::unordered_map<const char*, string> cache;
    auto it = cache.find(mangled_name);
    if (it != cache.end()) return it->second;
#if defined(__GNUG__) || defined(__clang__)
    // Demangle an ABI name to a string, if possible.
    // At worst, just pass the mangled name.
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
    string result = (status == 0 && demangled)
        ? string(demangled)
        : string(mangled_name);
    free(demangled);
#else
    // Not all compilers can demangle this way.
    string result(mangled_name);
#endif
    cache[mangled_name] = result;
    return result;
}

#ifdef STEAMSDK
#include "steam/steam_api.h"
#include "steam/steam_api_flat.h"
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#endif

#ifdef DEBUG
int DEBUG_PobjCount;
PObject* DEBUG_PobjListStart;
#endif

Engine* engine;

Engine::Engine()
{
    engine = this;

#ifdef __APPLE__
    // TODO: Find a proper solution.
    // Seems to be non-NULL even outside of a proper bundle.
    CFBundleRef bundle = CFBundleGetMainBundle();

    if (bundle)
    {
        char bundle_path[PATH_MAX], exe_path[PATH_MAX];

        CFURLRef bundleURL = CFBundleCopyBundleURL(bundle);
        CFURLGetFileSystemRepresentation(bundleURL, true, (unsigned char*)bundle_path, PATH_MAX);
        CFRelease(bundleURL);

        uint32_t size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &size) != 0)
            fprintf(stderr, "Failed to get executable path.\n");

        char *exe_realpath = realpath(exe_path, NULL);
        char *exe_dir      = dirname(exe_realpath);

        if (strcmp(exe_dir, bundle_path))
        {
            char resources_path[PATH_MAX];

            CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
            CFURLGetFileSystemRepresentation(resourcesURL, true, (unsigned char*)resources_path, PATH_MAX);
            CFRelease(resourcesURL);

            chdir(resources_path);
        }
        else chdir(exe_dir);

        free(exe_realpath);
        free(exe_dir);
    }
#endif

#ifdef STEAMSDK
    // 1907040 is EmptyEpsilon's Steam ID:
    // https://store.steampowered.com/app/1907040/EmptyEpsilon/
    if (SteamAPI_RestartAppIfNecessary(1907040)) exit(1);

    if (!SteamAPI_Init())
    {
        LOG(Error, "Failed to initialize Steam API.");
        exit(1);
    }

    SteamNetworkingUtils()->InitRelayNetworkAccess();
    LOG(Debug, "SteamID: ", SteamAPI_ISteamUser_GetSteamID(SteamAPI_SteamUser()));
#endif

#ifdef WIN32
    // Setup crash reporter (Dr. MinGW) if available.
    exchndl = DynamicLibrary::open("exchndl.dll");
    if (exchndl)
    {
        auto pfnExcHndlInit = exchndl->getFunction<void(*)(void)>("ExcHndlInit");

        if (pfnExcHndlInit)
        {
            pfnExcHndlInit();
            LOG(Info, "Crash reporter ON");
        }
        else exchndl.reset();
    }
#endif // WIN32

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // Enable mouse events on focus-grabbing clicks for multimonitor mode.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
    {
        const char* sdl_error{SDL_GetError()};
        LOG(Error, "SDL error in Engine initialization: ", sdl_error);
    }
    SDL_HideCursor();

    atexit(SDL_Quit);

    initRandom();
    soundManager = new SoundManager();
}

Engine::~Engine()
{
    Window::all_windows.clear();
    updatableList.clear();
    delete soundManager;
    soundManager = nullptr;
}

void Engine::registerObject(string name, P<PObject> obj)
{
    objectMap[name] = obj;
}

P<PObject> Engine::getObject(string name)
{
    if (!objectMap[name]) return NULL;
    return objectMap[name];
}

void Engine::runMainLoop()
{
    // There are no windows, so assume headless.
    if (Window::all_windows.size() == 0)
    {
        sp::SystemStopwatch frame_timer;
#ifdef DEBUG
        sp::SystemTimer debug_output_timer;
        debug_output_timer.repeat(5);
#endif

        while (running)
        {
            // Handle SDL_EVENT_QUIT event
            SDL_Event event;
            while (SDL_PollEvent(&event))
                if (event.type == SDL_EVENT_QUIT) running = false;
#ifdef DEBUG
            if (debug_output_timer.isExpired())
                LOG(DEBUG) << "Object count: " << DEBUG_PobjCount << " " << updatableList.size();
#endif

            auto realtime_delta = frame_timer.restart();
            auto update_delta = realtime_delta;
            update_delta = std::clamp(update_delta, 0.001f, 0.5f) * game_speed;

            EngineTiming engine_timing;
            sp::SystemStopwatch engine_timing_stopwatch;

            // Collect timings for engine systems and server_update, if enabled.
            // Otherwise, just update them.
            if (collect_engine_timing)
            {
                foreach (Updatable, u, updatableList)
                {
                    auto& u_deref = **u;
                    auto name = demangle(typeid(u_deref).name());
                    u->update(update_delta);
                    engine_timing["update:" + name] = engine_timing_stopwatch.restart();
                }

                for (auto system : systems)
                {
                    system->update(update_delta);
                    engine_timing[demangle(typeid(*system).name())] = engine_timing_stopwatch.restart();
                }

                sp::CollisionSystem::update(update_delta);
                engine_timing["collision"] = engine_timing_stopwatch.restart();
                elapsed_time += update_delta;

                engine_timing["server_update"] = 0.0f;
                if (game_server.isAlive()) engine_timing["server_update"] = game_server->getUpdateTime();
            }
            else
            {
                foreach (Updatable, u, updatableList) u->update(update_delta);
                for (auto system : systems) system->update(update_delta);
                sp::CollisionSystem::update(update_delta);
                elapsed_time += update_delta;
            }

            last_engine_timing = engine_timing;
            soundManager->updateTick();
#ifdef STEAMSDK
            SteamAPI_RunCallbacks();
#endif
            std::this_thread::sleep_for(std::chrono::duration<float>(0.016667f - realtime_delta));
        }
    }
    // Otherwise, assume EE isn't running headless.
    else
    {
        sp::audio::Source::startAudioSystem();
        sp::SystemStopwatch frame_timer;
#ifdef DEBUG
        // Dump object count every 5 seconds.
        sp::SystemTimer debug_output_timer;
        debug_output_timer.repeat(5);
#endif
        while (running)
        {
            // Handle events
            SDL_Event event;
            while (SDL_PollEvent(&event)) handleEvent(event);

#ifdef DEBUG
            if (debug_output_timer.isExpired())
                LOG(Debug, "Object count: ", DEBUG_PobjCount, " ", updatableList.size());
#endif

            float delta = frame_timer.restart();
            delta = std::clamp(delta, 0.001f, 0.5f) * game_speed;
            EngineTiming engine_timing;
            sp::SystemStopwatch engine_timing_stopwatch;

            // Collect Updatable, system, and collision timings if engine timing
            // collection is enabled. Otherwise, just update them.
            if (collect_engine_timing)
            {
                foreach (Updatable, u, updatableList)
                {
                    auto& u_deref = **u;
                    auto name = demangle(typeid(u_deref).name());
                    u->update(delta);
                    engine_timing["update:" + name] = engine_timing_stopwatch.restart();
                }

                for (auto system : systems)
                {
                    system->update(delta);
                    engine_timing[demangle(typeid(*system).name())] = engine_timing_stopwatch.restart();
                }

                elapsed_time += delta;
                sp::CollisionSystem::update(delta);
                engine_timing["collision"] = engine_timing_stopwatch.restart();
            }
            else
            {
                foreach (Updatable, u, updatableList) u->update(delta);
                for (auto system : systems) system->update(delta);
                elapsed_time += delta;
                sp::CollisionSystem::update(delta);
            }

            soundManager->updateTick();
#ifdef STEAMSDK
            SteamAPI_RunCallbacks();
#endif

            // Clear the window.
            for (auto window : Window::all_windows) window->render();

            // Collect rendering and server_update timings if engine timing
            // collection is enabled. Otherwise, just update the window.
            if (collect_engine_timing)
            {
                engine_timing["rendering"] = engine_timing_stopwatch.restart();
                for (auto window : Window::all_windows) window->swapBuffers();
                engine_timing_stopwatch.restart(); // skip vsync interval in timing

                engine_timing["server_update"] = 0.0f;
                if (game_server.isAlive())
                    engine_timing["server_update"] = game_server->getUpdateTime();
            }
            else
                for (auto window : Window::all_windows) window->swapBuffers();

            last_engine_timing = engine_timing;

            sp::io::Keybinding::allPostUpdate();
        }

        soundManager->stopMusic();
        sp::audio::Source::stopAudioSystem();
    }
}

void Engine::handleEvent(SDL_Event& event)
{
    // Stop running if SDL_EVENT_QUIT fires.
    if (event.type == SDL_EVENT_QUIT) running = false;
#ifdef DEBUG
    // Stop running if Escape is pressed anywhere in Debug builds.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
        running = false;

    // Dump list of objects if L key is pressed outside of text input.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_L
        && (!event.key.windowID || !SDL_TextInputActive(SDL_GetWindowFromID(event.key.windowID))))
    {
        int n = 0;
        printf("------------------------\n");
        std::unordered_map<string,int> totals;

        for (PObject* obj = DEBUG_PobjListStart; obj; obj = obj->DEBUG_PobjListNext)
        {
            printf("%c%4d: %4d: %s\n", obj->isDestroyed() ? '>' : ' ', n++, obj->getRefCount(), demangle(typeid(*obj).name()).c_str());
            if (!obj->isDestroyed())
                totals[demangle(typeid(*obj).name())] = totals[demangle(typeid(*obj).name())] + 1;
        }

        printf("--non-destroyed totals--\n");
        int grand_total = 0;

        for (auto entry : totals)
        {
            printf("%4d %s\n", entry.second, entry.first.c_str());
            grand_total += entry.second;
        }

        printf("%4d %s\n", grand_total, "All PObjects");
        printf("------------------------\n");

        sp::ecs::Entity::dumpDebugInfo();
        sp::ecs::ComponentStorageBase::dumpDebugInfo();
    }
#endif

    unsigned int window_id = 0;
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
#ifdef __EMSCRIPTEN__
        if (!audio_started)
        {
            sp::audio::AudioSource::startAudioSystem();
            audio_started = true;
        }
#endif
    case SDL_EVENT_KEY_UP:
        window_id = event.key.windowID;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        window_id = event.motion.windowID;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
#ifdef __EMSCRIPTEN__
        if (!audio_started)
        {
            sp::audio::AudioSource::startAudioSystem();
            audio_started = true;
        }
#endif
    case SDL_EVENT_MOUSE_BUTTON_UP:
        window_id = event.button.windowID;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        window_id = event.wheel.windowID;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_HIT_TEST:
    case SDL_EVENT_WINDOW_ICCPROF_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        window_id = event.window.windowID;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        window_id = event.tfinger.windowID;
        break;
    case SDL_EVENT_TEXT_EDITING:
        window_id = event.edit.windowID;
        break;
    case SDL_EVENT_TEXT_INPUT:
        window_id = event.text.windowID;
        break;
    }

    if (window_id != 0)
    {
        foreach (Window, window, Window::all_windows)
        {
            if (window->window && SDL_GetWindowID(static_cast<SDL_Window*>(window->window)) == window_id)
                window->handleEvent(event);
        }
    }

    sp::io::Keybinding::handleEvent(event);
}

void Engine::setGameSpeed(float speed)
{
    game_speed = speed;
}

float Engine::getGameSpeed()
{
    return game_speed;
}

float Engine::getElapsedTime()
{
    return elapsed_time;
}

Engine::EngineTiming Engine::getEngineTiming()
{
    return last_engine_timing;
}

void Engine::shutdown()
{
    running = false;
}
