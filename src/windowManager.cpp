#include "windowManager.h"

#ifdef _WIN32
#include "dynamicLibrary.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "graphics/opengl.h"
#include "graphics/image.h"
#include "resources.h"
#include "Updatable.h"
#include "Renderable.h"
#include "postProcessManager.h"
#include "io/keybinding.h"

#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <filesystem>
#include <thread>
#include <SDL3/SDL.h>
#include <stdlib.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

sp::io::Keybinding fullscreen_key{"FULLSCREEN"};
sp::io::Keybinding screenshot_key{"SCREENSHOT"};

static string getFromEnvironment(const char* key, string default_value) {
    auto value = getenv(key);
    if (!value)
        return default_value;
    return value;
}

PVector<Window> Window::all_windows;
SDL_GLContext Window::gl_context = nullptr;

Window::Window(glm::vec2 virtual_size, Mode mode, RenderChain* render_chain, int fsaa)
: minimal_virtual_size(virtual_size), current_virtual_size(virtual_size), render_chain(render_chain), mode(mode), fsaa(fsaa)
{
    srand(static_cast<int32_t>(time(nullptr)));

#ifdef _WIN32
    //On Vista or newer windows, let the OS know we are DPI aware, so we won't have odd scaling issues.
    auto user32 = DynamicLibrary::open("USER32.DLL");
    if (user32)
    {
        auto SetProcessDPIAware = user32->getFunction<BOOL(WINAPI *)(void)>("SetProcessDPIAware");
        if (SetProcessDPIAware)
            SetProcessDPIAware();
    }
#endif

    create();
    sp::initOpenGL();

    // Enable multisampling after OpenGL is initialized.
    switch (fsaa)
    {
    case 0:
        // Expected input for no FSAA.
        break;
    case 2:
    case 4:
    case 8:
        // Expected inputs for FSAA.
        glEnable(GL_MULTISAMPLE);
        break;
    default:
        LOG(Warning, "FSAA must be off (0), 2x, 4x, or 8x, but ", fsaa , "x was passed and ignored.");
    }

    all_windows.push_back(this);
}

Window::~Window()
{
    if (gl_context && all_windows.size() <= 1)
        SDL_GL_DestroyContext(gl_context);
    if (window)
        SDL_DestroyWindow(window);
}

void Window::render()
{
    if (fullscreen_key.getDown())
        setMode(getMode() == Mode::Window ? Mode::Fullscreen : Mode::Window);

    SDL_GL_MakeCurrent(window, gl_context);

    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    glViewport(0, 0, w, h);

    // Clear the window
    glClearColor(0.1f, 0.1f, 0.1f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    //Call the first item of the rendering chain.
    sp::RenderTarget target{current_virtual_size, {w, h}};
    render_chain->render(target);
    target.finish();

    // If multimonitor, trigger screenshots only on the first window.
    if (*all_windows.front() == this && screenshot_key.getDown())
        saveAllScreenshotsToFile();
}

void Window::swapBuffers()
{
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SwapWindow(window);
}

void Window::saveAllScreenshotsToFile()
{
    // Generate a shared timestamp so all windows in this batch share a base
    // name.
    auto now = time(nullptr);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "screenshot_%Y%m%d_%H%M%S", localtime(&now));

    bool multiple = all_windows.size() > 1;
    int index = 0;

    // Set GL_PACK_ALIGNMENT to 1 to avoid a buffer overflow. The default value
    // is 4 bytes, but the width isn't necessarily divisible by that. This is a
    // performance hit, but only screenshots use glReadPixels.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (auto w : all_windows)
    {
        // Make this window's GL context current to read its back buffer.
        SDL_GL_MakeCurrent(w->window, gl_context);

        // Capture this window's width and height.
        int width, height;
        SDL_GetWindowSizeInPixels(w->window, &width, &height);

        auto pixels = std::make_shared<std::vector<unsigned char>>(width * height * 3);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels->data());

        // Add a _N index suffix when there are multiple windows.
        char filename[80];
        if (multiple)
            snprintf(filename, sizeof(filename), "%s_%d.png", timestamp, index);
        else
            snprintf(filename, sizeof(filename), "%s.png", timestamp);

        // Launch a background thread to flip, encode, and write without blocking render.
        std::thread([pixels, width, height, filename]()
        {
            // OpenGL origin is bottom-left, PNG origin is top-left, so flip vertically.
            std::vector<unsigned char> flipped(width * height * 3);
            for (int y = 0; y < height; y++)
                memcpy(&flipped[y * width * 3], &(*pixels)[(height - 1 - y) * width * 3], width * 3);

            const string output_directory = ".";
            string full_path = output_directory + "/" + filename;

            if (stbi_write_png(full_path.c_str(), width, height, 3, flipped.data(), width * 3))
                LOG(Info, "Screenshot saved to ", full_path);
            else
                LOG(Error, "Failed to save screenshot to ", full_path);
        }).detach();

        index++;
    }
}

void Window::setMode(Mode new_mode)
{
    if (mode == new_mode)
        return;
    mode = new_mode;
    auto size = calculateWindowSize();
    SDL_SetWindowSize(window, size.x, size.y);
    switch(mode)
    {
    case Mode::Window:
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        break;
    case Mode::Fullscreen:
        SDL_SetWindowFullscreenMode(window, nullptr);
        SDL_SetWindowFullscreen(window, true);
        break;
    case Mode::ExclusiveFullscreen:
        {
            int num_display_modes = 0;
            SDL_DisplayID display = SDL_GetDisplayForWindow(window);
            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &num_display_modes);
            if (num_display_modes > 0)
                SDL_SetWindowFullscreenMode(window, modes[0]);
            SDL_free(modes);
            SDL_SetWindowFullscreen(window, true);
        }
        break;
    }
    setupView();
}

void Window::setFSAA(int new_fsaa)
{
    if (fsaa == new_fsaa) return;

    switch (new_fsaa)
    {
        case 0:
        case 2:
        case 4:
        case 8:
            fsaa = new_fsaa;
            break;
        default:
            LOG(Warning, "FSAA must be off (0), 2x, 4x, or 8x, but ", new_fsaa , "x was passed. Treating as fsaa=0.");
            if (fsaa == 0) return;
            fsaa = 0;
    }

    // Can't apply this without recreating the OpenGL context.
    // Log that a restart is required.
    LOG(Warning, "FSAA changed to ", fsaa, "x. Restart required for this change to take effect.");
}

void Window::setTitle(string title)
{
    SDL_SetWindowTitle(window, title.c_str());
}

void Window::setIcon(string icon_name)
{
    sp::Image image;
    if (!image.loadFromStream(getResourceStream(icon_name)))
    {
        LOG(Warning, "Couldn't load application icon ", icon_name);
        return;
    }

    auto size = image.getSize();
    SDL_Surface* icon_surface = SDL_CreateSurfaceFrom(
        size.x, size.y,
        SDL_PIXELFORMAT_RGBA32,
        image.getPtr(),
        size.x * 4
    );

    if (!icon_surface)
    {
        LOG(Warning, "Couldn't create SDL surface for application icon ", icon_name, ". SDL_Error: ", SDL_GetError());
        return;
    }

    // Set the window icon, then free the surface.
    SDL_SetWindowIcon(window, icon_surface);
    SDL_DestroySurface(icon_surface);
}

glm::vec2 Window::mapPixelToCoords(const glm::ivec2 point) const
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    float x = float(point.x) / float(w) * float(current_virtual_size.x);
    float y = float(point.y) / float(h) * float(current_virtual_size.y);
    return glm::vec2(x, y);
}

glm::ivec2 Window::mapCoordsToPixel(const glm::vec2 point) const
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    float x = float(point.x) * float(w) / float(current_virtual_size.x);
    float y = float(point.y) * float(h) / float(current_virtual_size.y);
    return glm::ivec2(x, y);
}

void Window::create()
{
    if (window) return;

    int display_nr = 0;
    for(auto w : all_windows)
    {
        if (w == this)
            break;
        display_nr ++;
    }

    // Create the window of the application
    auto size = calculateWindowSize();

#if defined(ANDROID)
    auto context_profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
    auto context_profile_minor_version = getFromEnvironment("SP_GL_MINOR", "0").toInt();
#elif defined(__APPLE__)
    auto context_profile_mask = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
    auto context_profile_minor_version = getFromEnvironment("SP_GL_MINOR", "1").toInt();
#else
    auto context_profile_mask = SDL_GL_CONTEXT_PROFILE_CORE;
    auto context_profile_minor_version = getFromEnvironment("SP_GL_MINOR", "1").toInt();
#endif
    if (getFromEnvironment("SP_GL_PROFILE", "") == "ES")
        context_profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
    else if (getFromEnvironment("SP_GL_PROFILE", "") == "CORE")
        context_profile_mask = SDL_GL_CONTEXT_PROFILE_CORE;
    else if (getFromEnvironment("SP_GL_PROFILE", "") == "COMPAT")
        context_profile_mask = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, context_profile_mask);
#if defined(DEBUG)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, getFromEnvironment("SP_GL_MAJOR", "2").toInt());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, context_profile_minor_version);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Configure multisampling for FSAA.
    switch (fsaa)
    {
    case 0:
        // Expected value for no FSAA.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        break;
    case 2:
    case 4:
    case 8:
        // Expected value for FSAA.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, fsaa);
        break;
    default:
        LOG(Warning, "FSAA must be off (0), 2x, 4x, or 8x, but ", fsaa , "x was passed. Treating as fsaa=0.");
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    int num_displays = 0;
    SDL_DisplayID* display_ids = SDL_GetDisplays(&num_displays);
    SDL_DisplayID target_display = num_displays > display_nr ? display_ids[display_nr] : 0;
    SDL_free(display_ids);

    int flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, size.x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, size.y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);

    if (!window)
    {
        LOG(Error, "Failed to create SDL window: ", SDL_GetError());
        exit(1);
    }

    if (mode != Mode::Window)
    {
        if (mode == Mode::ExclusiveFullscreen)
        {
            int num_display_modes = 0;
            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(target_display, &num_display_modes);
            if (num_display_modes > 0)
                SDL_SetWindowFullscreenMode(window, modes[0]);
            SDL_free(modes);
        }
        SDL_SetWindowFullscreen(window, true);
    }

    if (!gl_context)
    {
        gl_context = SDL_GL_CreateContext(window);
        if (!gl_context)
        {
            SDL_DestroyWindow(window);
            LOG(Warning, "Failed to create OpenGL context: ", SDL_GetError());
            LOG(Info, "retrying with GLES2.0 context");
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_PropertiesID retry_props = SDL_CreateProperties();
            SDL_SetStringProperty(retry_props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "");
            SDL_SetNumberProperty(retry_props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
            SDL_SetNumberProperty(retry_props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
            SDL_SetNumberProperty(retry_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, size.x);
            SDL_SetNumberProperty(retry_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, size.y);
            SDL_SetNumberProperty(retry_props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags);
            window = SDL_CreateWindowWithProperties(retry_props);
            SDL_DestroyProperties(retry_props);
            gl_context = SDL_GL_CreateContext(window);
        }

        if (!gl_context)
        {
            LOG(Error, "Failed to create OpenGL context:", SDL_GetError());
            exit(1);
        }
    }

    SDL_GL_MakeCurrent(window, gl_context);
    if (!SDL_GL_SetSwapInterval(-1)) SDL_GL_SetSwapInterval(1);

    // Log FSAA status.
    if (fsaa > 0)
    {
        int actual_buffers, actual_samples;
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &actual_buffers);
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &actual_samples);

        if (actual_buffers == 0 || actual_samples == 0)
            LOG(Warning, "FSAA ", fsaa, "x requested but not available on this system.");
        else if (actual_samples != fsaa)
            LOG(Warning, "FSAA ", fsaa, "x requested, but ", actual_samples, "x provided.");
        else
            LOG(Info, "FSAA ", fsaa, "x enabled.");
    }

    setupView();
}

void Window::handleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            sp::io::Pointer::Button button = sp::io::Pointer::Button::Unknown;
            switch (event.button.button)
            {
            case SDL_BUTTON_LEFT: button = sp::io::Pointer::Button::Left; break;
            case SDL_BUTTON_MIDDLE: button = sp::io::Pointer::Button::Middle; break;
            case SDL_BUTTON_RIGHT: button = sp::io::Pointer::Button::Right; break;
            default: break;
            }
            mouse_button_down_mask |= 1 << int(event.button.button);
            render_chain->onPointerDown(button, mapPixelToCoords({event.button.x, event.button.y}), sp::io::Pointer::mouse);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_button_down_mask)
            render_chain->onPointerDrag(mapPixelToCoords({event.motion.x, event.motion.y}), sp::io::Pointer::mouse);
        else
            render_chain->onPointerMove(mapPixelToCoords({event.motion.x, event.motion.y}), sp::io::Pointer::mouse);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        mouse_button_down_mask &=~(1 << int(event.button.button));
        render_chain->onPointerUp(mapPixelToCoords({event.button.x, event.button.y}), sp::io::Pointer::mouse);
        if (!mouse_button_down_mask)
            render_chain->onPointerMove(mapPixelToCoords({event.button.x, event.button.y}), sp::io::Pointer::mouse);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        render_chain->onMouseWheelScroll(mapPixelToCoords({static_cast<int>(event.wheel.mouse_x), static_cast<int>(event.wheel.mouse_y)}), event.wheel.y);
        break;
    case SDL_EVENT_FINGER_DOWN:
        render_chain->onPointerDown(sp::io::Pointer::Button::Touch, {event.tfinger.x * current_virtual_size.x, event.tfinger.y * current_virtual_size.y}, event.tfinger.fingerID);
        break;
    case SDL_EVENT_FINGER_MOTION:
        render_chain->onPointerDrag({event.tfinger.x * current_virtual_size.x, event.tfinger.y * current_virtual_size.y}, event.tfinger.fingerID);
        break;
    case SDL_EVENT_FINGER_UP:
        render_chain->onPointerUp({event.tfinger.x * current_virtual_size.x, event.tfinger.y * current_virtual_size.y}, event.tfinger.fingerID);
        break;
    case SDL_EVENT_TEXT_INPUT:
        render_chain->onTextInput(event.text.text);
        break;
    case SDL_EVENT_KEY_DOWN:
        switch(event.key.key)
        {
        case SDLK_KP_4:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_LEFT:
            if (event.key.mod & SDL_KMOD_SHIFT && event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::WordLeftWithSelection);
            else if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::WordLeft);
            else if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::LeftWithSelection);
            else
                render_chain->onTextInput(sp::TextInputEvent::Left);
            break;
        case SDLK_KP_6:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_RIGHT:
            if (event.key.mod & SDL_KMOD_SHIFT && event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::WordRightWithSelection);
            else if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::WordRight);
            else if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::RightWithSelection);
            else
                render_chain->onTextInput(sp::TextInputEvent::Right);
            break;
        case SDLK_KP_8:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_UP:
            if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::UpWithSelection);
            else
                render_chain->onTextInput(sp::TextInputEvent::Up);
            break;
        case SDLK_KP_2:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_DOWN:
            if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::DownWithSelection);
            else
                render_chain->onTextInput(sp::TextInputEvent::Down);
            break;
        case SDLK_KP_7:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_HOME:
            if (event.key.mod & SDL_KMOD_SHIFT && event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::TextStartWithSelection);
            else if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::TextStart);
            else if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::LineStartWithSelection);
            else
                render_chain->onTextInput(sp::TextInputEvent::LineStart);
            break;
        case SDLK_KP_1:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_END:
            if (event.key.mod & SDL_KMOD_SHIFT && event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::TextEndWithSelection);
            else if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::TextEnd);
            else if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::LineEndWithSelection);
            else
                render_chain->onTextInput(sp::TextInputEvent::LineEnd);
            break;
        case SDLK_KP_PERIOD:
            if (event.key.mod & SDL_KMOD_NUM)
                break;
            //fallthrough
        case SDLK_DELETE:
            render_chain->onTextInput(sp::TextInputEvent::Delete);
            break;
        case SDLK_BACKSPACE:
            render_chain->onTextInput(sp::TextInputEvent::Backspace);
            break;
        case SDLK_KP_ENTER:
        case SDLK_RETURN:
            if (event.key.mod & SDL_KMOD_ALT)
                setMode(getMode() == Mode::Window ? Mode::Fullscreen : Mode::Window);
            else if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::ReturnWithNewline);
            else
                render_chain->onTextInput(sp::TextInputEvent::Return);
            break;
        case SDLK_TAB:
        case SDLK_KP_TAB:
            if (event.key.mod & SDL_KMOD_SHIFT)
                render_chain->onTextInput(sp::TextInputEvent::Unindent);
            else
                render_chain->onTextInput(sp::TextInputEvent::Indent);
            break;
        case SDLK_A:
            if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::SelectAll);
            break;
        case SDLK_C:
            if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::Copy);
            break;
        case SDLK_V:
            if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::Paste);
            break;
        case SDLK_X:
            if (event.key.mod & SDL_KMOD_CTRL)
                render_chain->onTextInput(sp::TextInputEvent::Cut);
            break;
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (!SDL_GetMouseState(nullptr, nullptr))
        {
            render_chain->onPointerLeave(-1);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (mouse_button_down_mask)
        {
            mouse_button_down_mask = 0;
                float mx_f, my_f;
                SDL_GetMouseState(&mx_f, &my_f);
                int mx = static_cast<int>(mx_f), my = static_cast<int>(my_f);
            render_chain->onPointerUp(mapPixelToCoords({mx, my}), sp::io::Pointer::mouse);
        }
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        //close();
        break;
    case SDL_EVENT_WINDOW_RESIZED:
        setupView();
        break;
    case SDL_EVENT_QUIT:
        //close();
        break;
    default:
        break;
    }
}

void Window::setupView()
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    glm::vec2 window_size{w, h};

    current_virtual_size = minimal_virtual_size;

    if (window_size.x / window_size.y > current_virtual_size.x / current_virtual_size.y)
        current_virtual_size.x = current_virtual_size.y / window_size.y * window_size.x;
    else
        current_virtual_size.y = current_virtual_size.x / window_size.x * window_size.y;
}

glm::ivec2 Window::calculateWindowSize() const
{
    int display_nr = 0;

    for (auto w : all_windows)
    {
        if (w == this) break;
        display_nr++;
    }

    int num_displays = 0;
    SDL_DisplayID* display_ids = SDL_GetDisplays(&num_displays);
    SDL_DisplayID target_display = num_displays > display_nr
        ? display_ids[display_nr]
        : 0;
    SDL_DisplayID first_display = num_displays > 0 ? display_ids[0] : 0;
    SDL_free(display_ids);

    // Create the window of the application
    auto windowWidth = static_cast<int>(minimal_virtual_size.x);
    auto windowHeight = static_cast<int>(minimal_virtual_size.y);

    SDL_Rect rect{0, 0, 0, 0};
    const glm::ivec2 fallback_dimensions{640, 480};
    const int fallback_size = 240;
    const bool display_bounds = SDL_GetDisplayBounds(target_display, &rect);

    // On failure or 0-sized rect, try the first display as fallback.
    if (!display_bounds || rect.w == 0 || rect.h == 0)
    {
        if (!display_bounds)
        {
            LOG(Debug, "SDL_GetDisplayBounds(target_display, &rect) returned false. target_display: ", target_display);
            const char* sdl_error{SDL_GetError()};
            LOG(Error, "SDL error in Window::calculateWindowSize() at SDL_GetDisplayBounds(target_display, &rect): ", sdl_error);
            SDL_ClearError();
        }
        else
        {
            LOG(Debug, "SDL_GetDisplayBounds(target_display, &rect) succeeded, but at least one rect dimension is still 0. target_display: ", target_display, ", rect.w,h: ", rect.w, ",", rect.h);
        }

        if (first_display != 0 && SDL_GetDisplayBounds(first_display, &rect))
        {
            if (rect.w == 0 || rect.h == 0)
                LOG(Debug, "SDL_GetDisplayBounds(first_display, &rect) succeeded, but at least one rect dimension is still 0. rect.w,h: ", rect.w, ",", rect.h);
        }
        else if (first_display != 0)
        {
            const char* sdl_error{SDL_GetError()};
            LOG(Error, "SDL error in Window::calculateWindowSize() at SDL_GetDisplayBounds(first_display, &rect): ", sdl_error);
            SDL_ClearError();
        }
    }

    // Warn if the rect is too small to use
    if (rect.w < fallback_size || rect.h < fallback_size) LOG(Warning, "SDL_GetDisplayBounds() returned a rect with at least one dimension < ", fallback_size, ": ", rect.w, ",", rect.h);

    if (mode != Mode::Window)
    {
        if (rect.w >= fallback_size && rect.h >= fallback_size)
            return {rect.w, rect.h};
        else
        {
            LOG(Debug, "Calculated window size has at least one dimension of < ", fallback_size, ": ", rect.w, ",", rect.h, "\nFalling back to ", fallback_dimensions.x, ",", fallback_dimensions.y, ".");
            return fallback_dimensions;
        }
    }

    int scale = 2;
    int count = 0;
    int max_attempts = 128;
    while ((windowWidth * scale < static_cast<int>(rect.w)
        && windowHeight * scale < static_cast<int>(rect.h))
        && count < max_attempts)
    {
        count++;
        scale++;
    }

    if (count >= max_attempts)
        LOG(Warning, "Window::calculateWindowSize() couldn't solve scale in ", max_attempts, "attempts: ", scale);

    windowWidth *= scale - 1;
    windowHeight *= scale - 1;
    LOG(Debug, "Window dimensions before scaling loop: ", windowWidth, ",", windowHeight);

    count = 0;
    max_attempts = 16;
    while ((windowWidth >= static_cast<int>(rect.w) || windowHeight >= static_cast<int>(rect.h) - 100) && count < max_attempts)
    {
        count++;
        windowWidth = static_cast<int>(std::floor(windowWidth * 0.9f));
        windowHeight = static_cast<int>(std::floor(windowHeight * 0.9f));
    }

    if (count >= max_attempts)
        LOG(Warning, "Window::calculateWindowSize() couldn't solve windowWidth and windowHeight in ", max_attempts, " attempts: ", windowWidth, ",", windowHeight);

    if (windowWidth < fallback_size || windowHeight < fallback_size)
    {
        LOG(Debug, "Window::calculateWindowSize() reported at least one window dimension of < ", fallback_size, ": ", windowWidth, ",", windowHeight, "\nFalling back to ", fallback_dimensions.x, ",", fallback_dimensions.y, ".");
        return fallback_dimensions;
    }

    return {windowWidth, windowHeight};
}
