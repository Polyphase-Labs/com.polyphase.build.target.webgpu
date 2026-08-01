/**
 * @file System_Web.cpp
 * @brief Web (Emscripten) implementation of the engine's SYS_* surface.
 *
 * Emscripten exposes a POSIX-ish libc over MEMFS (stdio, dirent, stat), so
 * most of this mirrors the engine's Linux path. Web specifics:
 *
 *   - Content ships in the MEMFS preload bundle (<name>.data): Config.ini,
 *     Content.pak / the loose cooked tree — all mounted under / before
 *     main() runs. Every read is synchronous stdio.
 *   - The build is SINGLE-THREADED: SYS_CreateThread returns nullptr (the
 *     engine's null-thread fallbacks drain work synchronously) and every
 *     mutex op is a no-op.
 *   - Time is emscripten_get_now() (millisecond float, sub-ms precision).
 *   - Saves live in MEMFS under /polyphase_save/ for now — they survive the
 *     session only. IDBFS + FS.syncfs persistence is a planned follow-up.
 *   - No process exec, no dialogs; window state lives on the canvas and is
 *     managed by Main_Web/Graphics_WebGL2.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "System/System.h"
#include "Engine.h"
#include "Stream.h"
#include "Log.h"
#include "Utilities.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/heap.h>   // emscripten_get_heap_size

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <utility>

static bool sInitialized = false;

// =========================================================================
// Lifecycle
// =========================================================================

void SYS_Initialize()
{
    if (sInitialized) return;
    sInitialized = true;

    // Session-local save root (MEMFS). IDBFS persistence is a follow-up.
    mkdir("/polyphase_save", 0777);

    LogDebug("System_Web: initialised (Emscripten, single-threaded)");
}

void SYS_Shutdown()
{
    sInitialized = false;
}

void SYS_Update()
{
    // Browser events are delivered via the emscripten callbacks registered in
    // Main_Web/Input_Web; nothing to pump here.
    GetEngineState()->mQuit = GetEngineState()->mQuit ||
                              GetEngineState()->mSystem.mQuitRequested;
}

// =========================================================================
// Paths — the MEMFS bundle is mounted at /, which is also the cwd.
// =========================================================================

std::string SYS_GetPolyphasePath()
{
    return "/";
}

std::string SYS_GetExecutablePath()
{
    return "/polyphase.wasm"; // notional; nothing opens it
}

std::string SYS_GetCurrentDirectoryPath()
{
    char buf[512] = {0};
    if (getcwd(buf, sizeof(buf) - 1) == nullptr) return "/";
    std::string out = buf;
    if (out.empty() || out.back() != '/') out += '/';
    return out;
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    if (!relativePath.empty() && relativePath[0] == '/') return relativePath;
    return "/" + relativePath;
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}

void SYS_SetWorkingDirectory(const std::string& dirPath)
{
    if (!dirPath.empty()) chdir(dirPath.c_str());
}

// =========================================================================
// File I/O — plain stdio over MEMFS. The engine hands paths relative to the
// app root ("Config.ini", "<Project>/Assets/...", "Engine/Assets/..."); the
// bundle mounts them at /, and cwd is /, so both relative and /-absolute
// forms resolve.
// =========================================================================

static std::string WebResolvePath(const char* path)
{
    if (path == nullptr || path[0] == '\0') return std::string();
    if (path[0] == '/') return std::string(path);
    return std::string("/") + path;
}

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    struct stat st;
    return stat(WebResolvePath(path).c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

void SYS_AcquireFileData(const char* path, bool /*isAsset*/, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    const std::string full = WebResolvePath(path);
    FILE* f = fopen(full.c_str(), "rb");
    if (f == nullptr)
    {
        LogWarning("SYS_AcquireFileData: fopen failed for '%s'", full.c_str());
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return; }

    uint32_t actualSize = (uint32_t)size;
    if (maxSize > 0 && actualSize > (uint32_t)maxSize) actualSize = (uint32_t)maxSize;

    outData = (char*)malloc(actualSize);
    if (outData == nullptr)
    {
        fclose(f);
        LogError("SYS_AcquireFileData: malloc(%u) failed for '%s'", actualSize, path);
        return;
    }

    const size_t read = fread(outData, 1, actualSize, f);
    fclose(f);
    outSize = (uint32_t)read;
}

void SYS_ReleaseFileData(char* data)
{
    free(data);
}

bool SYS_CreateDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return false;
    return mkdir(WebResolvePath(dirPath).c_str(), 0777) == 0;
}

void SYS_RemoveDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return;
    rmdir(WebResolvePath(dirPath).c_str());
}

// Directory iteration drains the whole listing into a heap vector up-front
// (see SystemTypes_Platform.h) and iterates the snapshot — shared shape with
// the console ports.
namespace
{
    using DirDrain = std::vector<std::pair<std::string, bool>>; // (name, isDir)
}

void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    outDirEntry.mValid = false;
    outDirEntry.mDirDrain = nullptr;
    outDirEntry.mDirIndex = 0;

    const std::string resolved = WebResolvePath(dirPath.c_str());
    DIR* dir = opendir(resolved.c_str());
    if (dir == nullptr) return;

    DirDrain* drain = new DirDrain();
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr)
    {
        if (ent->d_name[0] == '\0') continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        bool isDir = false;
        std::string full = resolved;
        if (!full.empty() && full.back() != '/') full += '/';
        full += ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) isDir = S_ISDIR(st.st_mode);
        drain->push_back(std::make_pair(std::string(ent->d_name), isDir));
    }
    closedir(dir);

    strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mDirectoryPath[MAX_PATH_SIZE] = '\0';
    outDirEntry.mDirDrain = drain;
    outDirEntry.mDirIndex = 0;
    outDirEntry.mValid = true;

    // Prime the first entry so callers get the same invariant as other
    // platforms: after Open, either mFilename holds the first name or !mValid.
    SYS_IterateDirectory(outDirEntry);
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    DirDrain* drain = static_cast<DirDrain*>(dirEntry.mDirDrain);
    if (drain == nullptr || dirEntry.mDirIndex >= drain->size())
    {
        dirEntry.mValid = false;
        return;
    }

    const std::pair<std::string, bool>& e = (*drain)[dirEntry.mDirIndex++];
    strncpy(dirEntry.mFilename, e.first.c_str(), MAX_PATH_SIZE);
    dirEntry.mFilename[MAX_PATH_SIZE] = '\0';
    dirEntry.mDirectory = e.second;
    dirEntry.mValid = true;
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    DirDrain* drain = static_cast<DirDrain*>(dirEntry.mDirDrain);
    delete drain;
    dirEntry.mDirDrain = nullptr;
    dirEntry.mDirIndex = 0;
    dirEntry.mValid = false;
}

bool SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return false;

    FILE* src = fopen(sourcePath, "rb");
    if (src == nullptr) return false;
    FILE* dst = fopen(destPath, "wb");
    if (dst == nullptr) { fclose(src); return false; }

    bool ok = true;
    char buf[4096];
    size_t read = 0;
    while ((read = fread(buf, 1, sizeof(buf), src)) > 0)
    {
        if (fwrite(buf, 1, read, dst) != read) { ok = false; break; }
    }
    fclose(src);
    fclose(dst);
    return ok;
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
void SYS_MoveDirectory(const char* sourceDir, const char* destDir)
{
    if (sourceDir && destDir) rename(sourceDir, destDir);
}
void SYS_MoveFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath && destPath) rename(sourcePath, destPath);
}
void SYS_RemoveFile(const char* path)
{
    if (path) remove(path);
}
bool SYS_Rename(const char* oldPath, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr) return false;
    return rename(oldPath, newPath) == 0;
}

std::vector<std::string> SYS_OpenFileDialog() { return {}; }
std::string SYS_SaveFileDialog() { return ""; }
std::string SYS_SelectFolderDialog() { return ""; }

std::string SYS_GetFileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// =========================================================================
// Threading — single-threaded wasm: no threads, no-op "mutexes".
// =========================================================================

ThreadObject* SYS_CreateThread(ThreadFuncFP /*func*/, void* /*arg*/)
{
    // No -pthread in this build. Returning nullptr routes the engine onto its
    // threadless fallbacks (AssetManager drains async loads synchronously in
    // Update()).
    return nullptr;
}

void SYS_JoinThread(ThreadObject* /*thread*/) {}
void SYS_DestroyThread(ThreadObject* /*thread*/) {}

// Mutexes hand out a real heap token (callers null-check them) but lock and
// unlock are no-ops — there is exactly one thread.
MutexObject* SYS_CreateMutex()
{
    return new MutexObject(1);
}

void SYS_LockMutex(MutexObject* /*mutex*/) {}
void SYS_UnlockMutex(MutexObject* /*mutex*/) {}

void SYS_DestroyMutex(MutexObject* mutex)
{
    delete mutex;
}

void SYS_Sleep(uint32_t /*milliseconds*/)
{
    // Blocking the single thread would freeze the tab; every engine call site
    // that sleeps is a worker-thread idle loop, none of which exist here.
}

// =========================================================================
// Time
// =========================================================================

uint64_t SYS_GetTimeMicroseconds()
{
    return (uint64_t)(emscripten_get_now() * 1000.0);
}

// =========================================================================
// Process exec — N/A in a browser. SystemUtils.cpp supplies ExecCommon /
// SYS_ExecFull (stubbed under POLYPHASE_PLATFORM_ADDON); just provide SYS_Exec.
// =========================================================================

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// =========================================================================
// Memory — wasm linear heap (growable; INITIAL_MEMORY is the floor).
// =========================================================================

void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;

    const uint32_t heapSize = (uint32_t)emscripten_get_heap_size();
    struct mallinfo mi = mallinfo();

    MemoryStat mainRam;
    mainRam.mName = "WasmHeap";
    mainRam.mBytesAllocated = (uint32_t)mi.uordblks;
    mainRam.mBytesFree = heapSize > (uint32_t)mi.uordblks
                             ? heapSize - (uint32_t)mi.uordblks : 0;
    stats.push_back(mainRam);

    return stats;
}

float SYS_GetRAMUsage()    { auto s = SYS_GetMemoryStats(); return s.empty() ? 0.0f : (float)s[0].mBytesAllocated; }
float SYS_GetVRAMUsage()   { return 0.0f; } // browser-managed; not queryable
float SYS_GetRAM1Usage()   { return SYS_GetRAMUsage(); }
float SYS_GetRAM2Usage()   { return 0.0f; }
float SYS_GetCPUUsage()    { return 0.0f; }
float SYS_GetTotalRAM()    { return (float)emscripten_get_heap_size(); }
float SYS_GetTotalVRAM()   { return 0.0f; }
float SYS_GetTotalRAM1()   { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()   { return 0.0f; }

// =========================================================================
// Save data — MEMFS /polyphase_save/ (session-local). IDBFS + FS.syncfs
// persistence is a planned follow-up; the call surface won't change.
// =========================================================================

namespace
{
    inline std::string SavePath(const char* saveName)
    {
        std::string p = "/polyphase_save/";
        p += saveName;
        return p;
    }
}

bool SYS_ReadSave(const char* saveName, Stream& outStream)
{
    if (saveName == nullptr) return false;
    if (!SYS_DoesSaveExist(saveName))
    {
        LogWarning("SYS_ReadSave: '%s' does not exist", saveName);
        return false;
    }
    outStream.ReadFile(SavePath(saveName).c_str(), /*isAsset=*/false);
    return outStream.GetSize() > 0;
}

bool SYS_WriteSave(const char* saveName, Stream& stream)
{
    if (saveName == nullptr) return false;
    const std::string path = SavePath(saveName);
    const bool ok = stream.WriteFile(path.c_str());
    if (ok) LogDebug("Save written: %s (%u bytes)", saveName, (unsigned)stream.GetSize());
    else    LogError("SYS_WriteSave: failed to write '%s'", path.c_str());
    return ok;
}

bool SYS_DoesSaveExist(const char* saveName)
{
    if (saveName == nullptr) return false;
    FILE* f = fopen(SavePath(saveName).c_str(), "rb");
    if (f == nullptr) return false;
    fclose(f);
    return true;
}

bool SYS_DeleteSave(const char* saveName)
{
    if (saveName == nullptr) return false;
    return remove(SavePath(saveName).c_str()) == 0;
}

void SYS_UnmountMemoryCard() {}

// =========================================================================
// Clipboard — write-only via the async Clipboard API; reads are not
// synchronously available to wasm, so GetClipboardText returns empty.
// =========================================================================

void SYS_SetClipboardText(const std::string& str)
{
    EM_ASM({
        try { navigator.clipboard && navigator.clipboard.writeText(UTF8ToString($0)); }
        catch (e) {}
    }, str.c_str());
}

std::string SYS_GetClipboardText() { return ""; }

// =========================================================================
// Logging / assertions / console — printf reaches the browser console via
// Module.print (shell.html).
// =========================================================================

void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, arg);

    switch (severity)
    {
        case LogSeverity::Error:   fprintf(stderr, "[E] %s\n", buf); break;
        case LogSeverity::Warning: fprintf(stderr, "[W] %s\n", buf); break;
        default:                   printf("[D] %s\n", buf);          break;
    }
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    fprintf(stderr, "ASSERT: %s at %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    emscripten_force_exit(1);
}

void SYS_Alert(const char* message)
{
    fprintf(stderr, "ALERT: %s\n", message);
}

void SYS_UpdateConsole() {}

int32_t SYS_GetPlatformTier()
{
    return 1; // Browsers on desktop-class hardware; tune later if tiers matter.
}

// =========================================================================
// Window — the browser canvas. Sizing is owned by Main_Web (fill-window
// policy + resize callback); these report the live EngineState values.
// =========================================================================

void SYS_SetWindowTitle(const char* title)
{
    if (title) emscripten_set_window_title(title);
}

void SYS_SetWindowIcon(const char* /*iconPath*/) {}

bool SYS_DoesWindowHaveFocus()
{
    return !GetEngineState()->mSystem.mInBackground;
}

void SYS_SetScreenOrientation(ScreenOrientation /*orientation*/) {}
ScreenOrientation SYS_GetScreenOrientation()
{
    return GetEngineState()->mWindowWidth >= GetEngineState()->mWindowHeight
               ? ScreenOrientation::Landscape : ScreenOrientation::Portrait;
}

void SYS_SetFullscreen(bool fullscreen)
{
    if (fullscreen)
    {
        EmscriptenFullscreenStrategy strategy = {};
        strategy.scaleMode = EMSCRIPTEN_FULLSCREEN_SCALE_STRETCH;
        strategy.canvasResolutionScaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_HIDEF;
        strategy.filteringMode = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT;
        emscripten_request_fullscreen_strategy("#canvas", EM_TRUE, &strategy);
    }
    else
    {
        emscripten_exit_fullscreen();
    }
}

bool SYS_IsFullscreen()
{
    EmscriptenFullscreenChangeEvent st = {};
    if (emscripten_get_fullscreen_status(&st) != EMSCRIPTEN_RESULT_SUCCESS) return false;
    return st.isFullscreen;
}

void SYS_SetWindowRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
void SYS_GetWindowRect(int32_t& outX, int32_t& outY, int32_t& outWidth, int32_t& outHeight)
{
    outX = 0;
    outY = 0;
    outWidth  = (int32_t)GetEngineState()->mWindowWidth;
    outHeight = (int32_t)GetEngineState()->mWindowHeight;
}
bool SYS_IsWindowMaximized() { return true; }
void SYS_MaximizeWindow() {}

#endif // POLYPHASE_PLATFORM_ADDON
