#include "resources.h"

#include <cstdio>
#include <filesystem>
#include <SDL3/SDL.h>

#ifdef ANDROID
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#endif

PVector<ResourceProvider> resourceProviders;

ResourceProvider::ResourceProvider()
{
    resourceProviders.push_back(this);
}

bool ResourceProvider::searchMatch(const string name, const string searchPattern)
{
    std::vector<string> parts = searchPattern.split("*");
    int pos = 0;
    if (parts[0].length() > 0)
        if (name.find(parts[0]) != 0) return false;

    for (unsigned int n = 1; n < parts.size(); n++)
    {
        int offset = name.find(parts[n], pos);
        if (offset < 0) return false;
        pos = offset + static_cast<int>(parts[n].length());
    }

    return pos == static_cast<int>(name.length());
}

string ResourceStream::readLine()
{
    string ret;
    char c;
    while (true)
    {
        if (read(&c, 1) < 1) return ret;
        if (c == '\n') return ret;
        ret += string(c);
    }
}

string ResourceStream::readAll()
{
    string result;
    result.resize(getSize());
    read(result.data(), result.size());
    return result;
}

class FileResourceStream : public ResourceStream
{
    SDL_IOStream *io;
    size_t size = 0;
public:
    FileResourceStream(string filename)
    {
#ifndef ANDROID
        std::error_code ec;
        // Error code "no such file or directory" is thrown often, so don't
        // trace here to avoid spamming the log.
        if (!std::filesystem::is_regular_file(filename.c_str(), ec))
            io = nullptr;
        else
            io = SDL_IOFromFile(filename.c_str(), "rb");
#else
       //Android reads from the assets bundle, so we cannot check if the file exists and is a regular file
       io = SDL_IOFromFile(filename.c_str(), "rb");
#endif
    }

    virtual ~FileResourceStream()
    {
        if (io) SDL_CloseIO(io);
    }

    bool isOpen()
    {
        return io != nullptr;
    }

    virtual size_t read(void* data, size_t size) override
    {
        return SDL_ReadIO(io, data, size);
    }

    virtual size_t seek(size_t position) override
    {
        auto offset = SDL_SeekIO(io, position, SDL_IO_SEEK_SET);
        SDL_assert(offset != -1);
        return static_cast<size_t>(offset);
    }

    virtual size_t tell() override
    {
        auto offset = SDL_SeekIO(io, 0, SDL_IO_SEEK_CUR);
        SDL_assert(offset != -1);
        return static_cast<size_t>(offset);
    }

    virtual size_t getSize() override
    {
        if (size == 0)
        {
            size_t cur = tell();
            auto end_offset = SDL_SeekIO(io, 0, SDL_IO_SEEK_END);
            SDL_assert(end_offset != -1);
            size = static_cast<size_t>(end_offset);
            seek(cur);
        }

        return size;
    }
};


DirectoryResourceProvider::DirectoryResourceProvider(string basepath)
{
    this->basepath = basepath.rstrip("\\/");
}

P<ResourceStream> DirectoryResourceProvider::getResourceStream(string filename)
{
    P<FileResourceStream> stream = new FileResourceStream(basepath + "/" + filename);
    if (stream->isOpen()) return stream;
    return nullptr;
}

std::vector<string> DirectoryResourceProvider::findResources(string searchPattern)
{
    std::vector<string> found_files;
#if defined(ANDROID)
    //Limitation :
    //As far as I know, Android NDK won't provide a way to list subdirectories
    //So we will only list files in the first level directory
    static jobject asset_manager_jobject;
    static AAssetManager* asset_manager = nullptr;
    if (!asset_manager)
    {
        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        jobject activity = (jobject)SDL_GetAndroidActivity();
        jclass clazz(env->GetObjectClass(activity));
        jmethodID method_id = env->GetMethodID(clazz, "getAssets", "()Landroid/content/res/AssetManager;");
        asset_manager_jobject = env->CallObjectMethod(activity, method_id);
        asset_manager = AAssetManager_fromJava(env, asset_manager_jobject);

        env->DeleteLocalRef(activity);
        env->DeleteLocalRef(clazz);
    }

    if(asset_manager)
    {
        int idx = searchPattern.rfind("/");
        string forced_path = basepath;
        string prefix = "";
        if (idx > -1)
        {
            prefix = searchPattern.substr(0, idx);
            forced_path += "/" + prefix;
            prefix += "/";
        }
        AAssetDir* dir = AAssetManager_openDir(asset_manager, (forced_path).c_str());
        if (dir)
        {
            const char* filename;
            while ((filename = AAssetDir_getNextFileName(dir)) != nullptr)
            {
                if (searchMatch(prefix + filename, searchPattern))
                    found_files.push_back(prefix + filename);
            }
            AAssetDir_close(dir);
        }
    }
#else
    namespace fs = std::filesystem;

    const fs::path root{ basepath.data() };
    constexpr auto traversal_options{ fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied };
    std::error_code error_code{};
    for (const auto& entry : fs::recursive_directory_iterator(root, traversal_options, error_code))
    {
        // Use relative generic paths (i.e. forward slashes) in case the caller
        // wants to pattern match with a folder.
        if (!error_code)
        {
            auto relative_path = entry.path().lexically_relative(root).generic_u8string();
            if (!entry.is_directory() && searchMatch(relative_path, searchPattern))
                found_files.push_back(relative_path);
        }
        else
            LOG(Warning, "[sp-drp] Resource path ", entry.path().u8string(), " encountered an error: ", error_code.message());
    }
#endif
    return found_files;
}

P<ResourceStream> getResourceStream(string filename)
{
    foreach (ResourceProvider, rp, resourceProviders)
    {
        P<ResourceStream> stream = rp->getResourceStream(filename);
        if (stream) return stream;
    }
    return NULL;
}

std::vector<string> findResources(string searchPattern)
{
    std::vector<string> foundFiles;

    foreach (ResourceProvider, rp, resourceProviders)
    {
        std::vector<string> res = rp->findResources(searchPattern);
        foundFiles.insert(foundFiles.end(), res.begin(), res.end());
    }

    return foundFiles;
}
