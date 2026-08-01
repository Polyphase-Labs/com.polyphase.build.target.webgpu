// MSVC's SDL deprecations on getenv/fopen/strncpy don't apply to this addon —
// every call is bounds-checked and pointer-validated. Suppress before the CRT
// headers come in. (The build systems also define this on the command line;
// guard to avoid a redefinition warning when they do.)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

/**
 * @file ComPolyphaseBuildTargetWebgpu.cpp
 * @brief Web (WebGPU / Emscripten) build-target addon for Polyphase Engine.
 *
 * Sibling of com.polyphase.build.target.webgl2 — same pipeline, different
 * renderer (Dawn's emdawnwebgpu port + WGSL instead of WebGL2/ESSL). The two
 * targets coexist in one project: distinct targetId (web.webgpu), profile-
 * option namespace (webgpu.*), staging dirs (Build/WebGPU, Intermediate/WebGPU)
 * and serve port (6932), so neither tramples the other's state.
 *
 * Adds a "Web (WebGPU)" entry to the editor's Build Profile dropdown and
 * drives compile (emcc -> .html/.js/.wasm) -> package (file_packager MEMFS
 * bundle + index.html) -> run (local HTTP server + default browser).
 *
 * Toolchain routing (per-profile, `web.toolchain`):
 *
 *   "emsdk-native" (default)
 *       Windows: writes a .bat that `call`s <emsdk>\emsdk_env.bat and runs
 *       GNU make (auto-probed from devkitPro's MSYS2 / MSYS2 installs, or a
 *       profile override) on Makefile_WebGPU. Paths stay in M:/... form, which
 *       both MSYS2 make and emcc accept.
 *       Linux/macOS: writes a .sh that sources <emsdk>/emsdk_env.sh and runs
 *       the system make.
 *
 *   "emsdk-wsl" (Windows fallback, emsdk installed inside WSL)
 *       Runs `wsl [-d <distro>] bash -lc "sh <script>"`. The emitted command
 *       starts with `wsl `, which flips the engine's AddonInject.mk path
 *       style to /mnt/<drive>/ automatically.
 *
 * The output is single-threaded WebAssembly: no pthreads, no
 * SharedArrayBuffer, no COOP/COEP headers — the packaged folder runs from any
 * static host.
 *
 * Licensing isolation: the engine binary never links Emscripten-specific
 * code. Every web-specific reference lives inside this addon DLL and its
 * Runtime/Web tree.
 *
 * Maintainer: Polyphase Engine team.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#include "imgui.h"
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

#if EDITOR
namespace
{
    // ----- Env / path helpers ----------------------------------------------

    std::string GetEnvOrEmpty(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(path, ec) && !std::filesystem::is_directory(path, ec);
    }

    bool DirExists(const std::string& path)
    {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::is_directory(path, ec);
    }

    // Translate a Windows absolute path to its default WSL2 mount:
    //   M:\Foo\Bar  ->  /mnt/m/Foo/Bar
    std::string WinToWslPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath;
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/mnt/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            return out;
        }
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }

    // Normalise a Windows path to forward slashes (MSYS2 make + emcc both
    // accept M:/Foo/Bar, and it avoids backslash-escape trouble in make).
    std::string FwdSlash(const std::string& p)
    {
        std::string out = p;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }

    // ----- Toolchain selection ---------------------------------------------

    enum class Toolchain { EmsdkNative, EmsdkWsl };

    // Per-profile option keys.
    constexpr const char* kToolchainKey  = "webgpu.toolchain";       // "emsdk-native" | "emsdk-wsl"
    constexpr const char* kEmsdkPathKey  = "webgpu.emsdkPath";       // emsdk root, in the build shell's namespace
    constexpr const char* kMakePathKey   = "webgpu.makePath";        // Windows native: GNU make executable
    constexpr const char* kWslDistroKey  = "webgpu.wslDistro";       // emsdk-wsl: `wsl -d <distro>`
    constexpr const char* kJobsKey       = "webgpu.jobs";            // make -j parallelism
    constexpr const char* kOptKey        = "webgpu.optLevel";        // "O0" | "O2" | "O3" | "Oz"
    constexpr const char* kMemoryKey     = "webgpu.initialMemoryMB"; // -sINITIAL_MEMORY
    constexpr const char* kMakefileKey   = "webgpu.makefile";        // bare filename in addon root, or absolute override
    constexpr const char* kPortKey       = "webgpu.serverPort";      // Build & Run local server port
    constexpr const char* kRelayUrlKey   = "webgpu.relayUrl";        // ws:// or wss:// multiplayer relay; empty = networking off

    constexpr const char* kJobsDefault     = "8";  // emcc TUs are far lighter than console gcc's
    constexpr const char* kOptDefault      = "O2";
    constexpr const char* kMemoryDefault   = "256";
    constexpr const char* kMakefileDefault = "Makefile_WebGPU";
    constexpr const char* kPortDefault     = "6932";

    std::string ReadOption(const PolyphaseBuildContext* ctx, const char* key, const char* fallback)
    {
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return fallback ? fallback : "";
        char buf[512] = {0};
        if (ctx->GetProfileSetting(key, buf, sizeof(buf)) == 0 || buf[0] == '\0')
            return fallback ? fallback : "";
        return std::string(buf);
    }

    Toolchain ResolveToolchain(const PolyphaseBuildContext* ctx)
    {
        std::string id = ReadOption(ctx, kToolchainKey, "");
        if (id == "emsdk-wsl") return Toolchain::EmsdkWsl;
        return Toolchain::EmsdkNative;
    }

    // Resolve the emsdk root: profile override, then $EMSDK, then common
    // install spots. Returned in the HOST namespace for native, and in the
    // WSL namespace for the wsl route (the profile value is authoritative
    // there because the Windows process can't see WSL's env).
    std::string ResolveEmsdkPath(const PolyphaseBuildContext* ctx, Toolchain tc)
    {
        std::string p = ReadOption(ctx, kEmsdkPathKey, "");
        if (!p.empty()) return p;
        if (tc == Toolchain::EmsdkWsl) return "$HOME/emsdk"; // expanded by the wsl shell
        p = GetEnvOrEmpty("EMSDK");
        if (!p.empty()) return p;
#if defined(_WIN32)
        for (const char* cand : { "C:\\emsdk", "C:\\tools\\emsdk", "C:\\devkitPro\\emsdk" })
            if (DirExists(cand)) return cand;
#else
        for (const char* cand : { "/opt/emsdk", "/usr/local/emsdk" })
            if (DirExists(cand)) return cand;
        std::string home = GetEnvOrEmpty("HOME");
        if (!home.empty() && DirExists(home + "/emsdk")) return home + "/emsdk";
#endif
        return "";
    }

#if defined(_WIN32)
    // Locate a GNU make for the Windows-native route. devkitPro's MSYS2 and
    // plain MSYS2 are probed first (the user's documented setups), then PATH.
    std::string ResolveMakeExe(const PolyphaseBuildContext* ctx)
    {
        std::string p = ReadOption(ctx, kMakePathKey, "");
        if (!p.empty()) return p;
        for (const char* cand : {
                 "C:\\devkitPro\\msys2\\usr\\bin\\make.exe",
                 "C:\\msys64\\usr\\bin\\make.exe",
                 "C:\\msys2\\usr\\bin\\make.exe",
                 "C:\\msys64\\mingw64\\bin\\mingw32-make.exe" })
            if (FileExists(cand)) return cand;
        return "make"; // hope it's on PATH
    }
#endif

    int ReadJobs(const PolyphaseBuildContext* ctx)
    {
        const std::string jobsOpt = ReadOption(ctx, kJobsKey, kJobsDefault);
        int jobs = 0;
        for (char c : jobsOpt) { if (c < '0' || c > '9') { jobs = 0; break; } jobs = jobs * 10 + (c - '0'); }
        if (jobs < 1 || jobs > 64) jobs = 8;
        return jobs;
    }

    int ReadPort(const PolyphaseBuildContext* ctx)
    {
        const std::string opt = ReadOption(ctx, kPortKey, kPortDefault);
        int port = std::atoi(opt.c_str());
        if (port < 1024 || port > 65535) port = 6932;
        return port;
    }

    std::string ReadOptLevel(const PolyphaseBuildContext* ctx)
    {
        std::string o = ReadOption(ctx, kOptKey, kOptDefault);
        if (o != "O0" && o != "O2" && o != "O3" && o != "Oz") o = kOptDefault;
        return o;
    }

    int ReadInitialMemoryMB(const PolyphaseBuildContext* ctx)
    {
        int mb = std::atoi(ReadOption(ctx, kMemoryKey, kMemoryDefault).c_str());
        if (mb < 64 || mb > 2048) mb = 256;
        return mb;
    }

    // Resolve the Makefile path (host form). Bare names live inside the addon;
    // absolute values are taken as-is.
    std::string ResolveMakefileHostPath(const PolyphaseBuildContext* ctx)
    {
        const std::string opt = ReadOption(ctx, kMakefileKey, kMakefileDefault);
        const bool isAbsolute =
            !opt.empty() && (opt[0] == '/' || (opt.size() >= 2 && opt[1] == ':'));
        if (isAbsolute) return opt;
        return std::string(ctx->projectDir) +
               "/Packages/com.polyphase.build.target.webgpu/" + opt;
    }

    // Single-quote a path for a POSIX shell body, escaping embedded quotes.
    std::string Sq(const std::string& s)
    {
        std::string out = "'";
        for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
        out += '\'';
        return out;
    }

    // ----- Validate (cached) -----------------------------------------------
    // Validate can be polled by the editor; keep it to file probes only.
    // (The PSP addon's shell-out froze the editor for up to 45 s.)

    int32_t sValidateCached = -1;             // -1 = unknown, 0/1 = last result
    std::string sValidateKey;
    char sValidateReason[512] = {0};

    int32_t Web_Validate(char* outReason, size_t cap)
    {
        // Validate has no ctx, so probe the profile-independent fallbacks
        // (the common case). Profile overrides still build correctly;
        // Validate is advisory.
        const std::string emsdk = []{
            std::string p = GetEnvOrEmpty("EMSDK");
#if defined(_WIN32)
            if (p.empty()) for (const char* c : { "C:\\emsdk", "C:\\tools\\emsdk", "C:\\devkitPro\\emsdk" })
                if (DirExists(c)) { p = c; break; }
#else
            if (p.empty()) for (const char* c : { "/opt/emsdk", "/usr/local/emsdk" })
                if (DirExists(c)) { p = c; break; }
            if (p.empty()) { std::string h = GetEnvOrEmpty("HOME"); if (!h.empty() && DirExists(h + "/emsdk")) p = h + "/emsdk"; }
#endif
            return p;
        }();

        std::string key = "E:" + emsdk;
        if (sValidateCached != -1 && key == sValidateKey)
        {
            std::snprintf(outReason, cap, "%s", sValidateReason);
            return sValidateCached;
        }
        sValidateKey = key;

        auto finish = [&](int32_t ok, const char* reason) -> int32_t {
            std::snprintf(sValidateReason, sizeof(sValidateReason), "%s", reason ? reason : "");
            std::snprintf(outReason, cap, "%s", sValidateReason);
            sValidateCached = ok;
            return ok;
        };

        if (!emsdk.empty() && FileExists(emsdk + "/upstream/emscripten/emcc.py"))
        {
#if defined(_WIN32)
            // Also need a GNU make for the native route.
            bool haveMake = false;
            for (const char* cand : {
                     "C:\\devkitPro\\msys2\\usr\\bin\\make.exe",
                     "C:\\msys64\\usr\\bin\\make.exe",
                     "C:\\msys2\\usr\\bin\\make.exe",
                     "C:\\msys64\\mingw64\\bin\\mingw32-make.exe" })
                if (FileExists(cand)) { haveMake = true; break; }
            if (!haveMake)
            {
                char msg[512];
                std::snprintf(msg, sizeof(msg),
                    "emsdk found at '%s' but no GNU make was found (probed devkitPro "
                    "MSYS2 and C:\\msys64). Install MSYS2, set the Target Options "
                    "'Make Path' field, or switch the toolchain to 'emsdk-wsl'.",
                    emsdk.c_str());
                return finish(0, msg);
            }
#endif
            return finish(1, "");
        }

#if defined(_WIN32)
        // No native emsdk — WSL emsdk still works if WSL itself exists.
        if (FileExists("C:\\Windows\\System32\\wsl.exe"))
        {
            return finish(1,
                "No native emsdk found (EMSDK unset, C:\\emsdk missing) — assuming "
                "emsdk inside WSL. Set the profile toolchain to 'emsdk-wsl' and the "
                "'Emsdk Path' field to its WSL location (default $HOME/emsdk).");
        }
        return finish(0,
            "Emscripten SDK not found. Install emsdk (git clone "
            "https://github.com/emscripten-core/emsdk && emsdk install latest && "
            "emsdk activate latest), set EMSDK, or point the profile's 'Emsdk Path' "
            "field at the install.");
#else
        return finish(0,
            "Emscripten SDK not found. Install emsdk and set EMSDK (or the "
            "profile's 'Emsdk Path' field) to its root.");
#endif
    }

    // ----- Compile ----------------------------------------------------------

    // Write the generated build script into Intermediate/WebGPU/ and return its
    // HOST path (empty on failure). Windows-native gets a .bat, everything
    // else a .sh.
    std::string WriteBuildScript(const PolyphaseBuildContext* ctx, Toolchain tc)
    {
        const std::string projectDir = ctx->projectDir;
        const std::string intHost    = projectDir + "/Intermediate/WebGPU";

        std::error_code ec;
        std::filesystem::create_directories(intHost, ec);
        if (ec) return "";

        const std::string emsdk   = ResolveEmsdkPath(ctx, tc);
        const std::string mkHost  = ResolveMakefileHostPath(ctx);
        const int jobs            = ReadJobs(ctx);
        const std::string opt     = ReadOptLevel(ctx);
        const int memMB           = ReadInitialMemoryMB(ctx);
        const std::string engHost = (ctx->engineDir && ctx->engineDir[0]) ? ctx->engineDir : "";

#if defined(_WIN32)
        if (tc == Toolchain::EmsdkNative)
        {
            const std::string scriptHost = intHost + "/polyphase_build.bat";
            const std::string makeExe    = ResolveMakeExe(ctx);

            std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return "";
            out << "@echo off\r\n";
            out << "setlocal\r\n";
            out << "call \"" << emsdk << "\\emsdk_env.bat\" >nul 2>&1\r\n";
            out << "if errorlevel 1 ( echo Failed to source emsdk_env.bat from \""
                << emsdk << "\" & exit /b 1 )\r\n";
            out << "cd /d \"" << intHost << "\"\r\n";
            if (ctx->forceRebuild)
                out << "del /q *.o *.d *.html *.js *.wasm 2>nul\r\n";
            out << "\"" << makeExe << "\" -f \"" << FwdSlash(mkHost) << "\""
                << " PROJECT_ROOT=\"" << FwdSlash(projectDir) << "\"";
            if (!engHost.empty())
                out << " POLYPHASE_PATH=\"" << FwdSlash(engHost) << "\"";
            out << " OPT=-" << opt
                << " INITIAL_MEMORY_MB=" << memMB
                << " -j" << jobs << "\r\n";
            out << "exit /b %errorlevel%\r\n";
            out.close();
            return out.good() ? scriptHost : std::string();
        }
#endif

        // POSIX script — used by Linux/macOS native AND the WSL route.
        const std::string scriptHost = intHost + "/polyphase_build.sh";
        const bool wsl = (tc == Toolchain::EmsdkWsl);
        auto shPath = [&](const std::string& host) {
            return wsl ? WinToWslPath(host) : host;
        };

        std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return "";
        out << "#!/bin/sh\n";
        out << "set -e\n";
        out << "export EMSDK_QUIET=1\n";
        // $HOME/emsdk-style defaults need the shell to expand them; a profile
        // override is a literal path.
        if (emsdk.find('$') != std::string::npos)
            out << ". \"" << emsdk << "/emsdk_env.sh\"\n";
        else
            out << ". " << Sq(shPath(emsdk)) << "/emsdk_env.sh\n";
        out << "mkdir -p " << Sq(shPath(intHost)) << "\n";
        out << "cd " << Sq(shPath(intHost)) << "\n";
        if (ctx->forceRebuild)
            out << "rm -f ./*.o ./*.d ./*.html ./*.js ./*.wasm 2>/dev/null || true\n";
        out << "emmake make -f " << Sq(shPath(mkHost))
            << " PROJECT_ROOT=" << Sq(shPath(projectDir));
        if (!engHost.empty())
            out << " POLYPHASE_PATH=" << Sq(shPath(engHost));
        out << " OPT=-" << opt
            << " INITIAL_MEMORY_MB=" << memMB
            << " -j" << jobs << "\n";
        out.close();
        return out.good() ? scriptHost : std::string();
    }

    // Emit the outer command that runs the generated script under the selected
    // shell. IMPORTANT: the emsdk-wsl command must start with "wsl " — the
    // engine sniffs that token to decide whether AddonInject.mk paths are
    // written as /mnt/<drive>/... (wsl) or M:/... (native).
    std::string BuildRunScriptCommand(const PolyphaseBuildContext* ctx, Toolchain tc,
                                      const std::string& scriptHost)
    {
#if defined(_WIN32)
        if (tc == Toolchain::EmsdkNative)
        {
            // The engine runs this via `cmd.exe /c <cmd>`. When a command
            // starts with a quoted path, cmd.exe strips the OUTER quote pair;
            // wrap in an extra pair so the real quoting survives.
            return "\"\"" + scriptHost + "\"\"";
        }
        std::string distro = ReadOption(ctx, kWslDistroKey, "");
        std::string wsl = distro.empty() ? "wsl " : ("wsl -d " + distro + " ");
        return wsl + "bash -lc \"sh " + Sq(WinToWslPath(scriptHost)) + "\"";
#else
        (void)ctx; (void)tc;
        return "bash " + Sq(scriptHost);
#endif
    }

    int32_t Web_GetCompileCommand(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr) return 0;

        // Embedded builds delete the cooked .oct/.lua from the package (the
        // engine expects them inside the binary via EmbeddedAssets.cpp), but
        // the Web runtime deliberately does NOT compile EmbeddedAssets.cpp —
        // assets come from the MEMFS bundle. The combination ships a package
        // with no assets at all, so refuse it with a clear message instead of
        // producing a black screen.
        if (ctx->embedded)
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                "WebGPU: the 'Embedded' profile option is not supported by this target — "
                "it strips the cooked assets the web bundle needs. Turn Embedded OFF "
                "(recommended: Static Content ON + Content Pak ON) and rebuild.");
            return 0;
        }

        const Toolchain tc = ResolveToolchain(ctx);
        const std::string scriptHost = WriteBuildScript(ctx, tc);
        if (scriptHost.empty())
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                "WebGPU: could not write Intermediate/WebGPU/polyphase_build script.");
            return 0;
        }

        const std::string cmd = BuildRunScriptCommand(ctx, tc, scriptHost);
        std::snprintf(outCmd, cap, "%s", cmd.c_str());
        return 1;
    }

    int32_t Web_GetCompiledBinaryPath(const PolyphaseBuildContext* ctx, char* outPath, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr || ctx->projectName == nullptr) return 0;
        // Makefile_WebGPU stages the emcc triple (.html/.js/.wasm) here; the
        // .html is the "binary" the engine copies. PostPackage brings the
        // .js/.wasm siblings along and assembles index.html.
        std::snprintf(outPath, cap, "%s/Build/WebGPU/%s.html",
                      ctx->projectDir, ctx->projectName);
        return 1;
    }

    // ----- Package ----------------------------------------------------------

    // Copy one file, overwriting. Returns false + logs on failure.
    bool CopyFileOverwrite(const PolyphaseBuildContext* ctx,
                           const std::string& src, const std::string& dst)
    {
        std::error_code ec;
        std::filesystem::copy_file(src, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec && ctx && ctx->Log)
        {
            char msg[768];
            std::snprintf(msg, sizeof(msg), "WebGPU: failed to copy '%s' -> '%s' (%s)",
                          src.c_str(), dst.c_str(), ec.message().c_str());
            ctx->Log(POLYPHASE_BT_LOG_ERROR, msg);
        }
        return !ec;
    }

    // Replace the first occurrence of `marker` in `path` with `replacement`.
    bool PatchFileMarker(const std::string& path, const std::string& marker,
                         const std::string& replacement)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        std::string text = ss.str();

        size_t pos = text.find(marker);
        if (pos == std::string::npos) return false;
        text.replace(pos, marker.size(), replacement);

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << text;
        return out.good();
    }

    // Build the file_packager --preload list from the packaged dir's top-level
    // entries, excluding the web module files themselves. One rule covers both
    // Static+ContentPak (Content.pak, Config.ini, Engine/, <name>/) and
    // loose-tree profiles.
    std::vector<std::string> GatherPreloadEntries(const std::string& packagedDir)
    {
        std::vector<std::string> entries;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(packagedDir, ec))
        {
            const std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            std::string lower = name;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            auto endsWith = [&](const char* suf) {
                size_t n = std::strlen(suf);
                return lower.size() >= n && lower.compare(lower.size() - n, n, suf) == 0;
            };
            if (endsWith(".html") || endsWith(".js") || endsWith(".wasm") ||
                endsWith(".data") || endsWith(".symbols"))
                continue;
            entries.push_back(name);
        }
        return entries;
    }

    // Write the packaging script that runs emsdk's file_packager.py over the
    // packaged content. Returns the HOST path (empty on failure).
    std::string WritePackageScript(const PolyphaseBuildContext* ctx, Toolchain tc,
                                   const std::vector<std::string>& entries)
    {
        const std::string projectDir  = ctx->projectDir ? ctx->projectDir : "";
        const std::string packagedDir = ctx->packageOutputDir;
        const std::string name        = ctx->projectName;
        const std::string intHost     = projectDir + "/Intermediate/WebGPU";
        std::error_code ec;
        std::filesystem::create_directories(intHost, ec);
        if (ec) return "";

        const std::string emsdk = ResolveEmsdkPath(ctx, tc);

#if defined(_WIN32)
        if (tc == Toolchain::EmsdkNative)
        {
            const std::string scriptHost = intHost + "/polyphase_package.bat";
            std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return "";
            out << "@echo off\r\n";
            out << "setlocal\r\n";
            out << "call \"" << emsdk << "\\emsdk_env.bat\" >nul 2>&1\r\n";
            out << "if not defined EMSDK_PYTHON set EMSDK_PYTHON=python\r\n";
            out << "cd /d \"" << packagedDir << "\"\r\n";
            out << "\"%EMSDK_PYTHON%\" \"%EMSDK%\\upstream\\emscripten\\tools\\file_packager.py\" "
                << "\"" << name << ".data\"";
            for (const std::string& e : entries)
                out << " --preload \"" << e << "@/" << e << "\"";
            out << " --js-output=\"" << name << ".data.js\"\r\n";
            out << "exit /b %errorlevel%\r\n";
            out.close();
            return out.good() ? scriptHost : std::string();
        }
#endif

        const std::string scriptHost = intHost + "/polyphase_package.sh";
        const bool wsl = (tc == Toolchain::EmsdkWsl);
        auto shPath = [&](const std::string& host) {
            return wsl ? WinToWslPath(host) : host;
        };

        std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return "";
        out << "#!/bin/sh\n";
        out << "set -e\n";
        out << "export EMSDK_QUIET=1\n";
        if (emsdk.find('$') != std::string::npos)
            out << ". \"" << emsdk << "/emsdk_env.sh\"\n";
        else
            out << ". " << Sq(shPath(emsdk)) << "/emsdk_env.sh\n";
        out << "cd " << Sq(shPath(packagedDir)) << "\n";
        out << "python3 \"$EMSDK/upstream/emscripten/tools/file_packager.py\" "
            << Sq(name + ".data");
        for (const std::string& e : entries)
            out << " --preload " << Sq(e + "@/" + e);
        out << " --js-output=" << Sq(name + ".data.js") << "\n";
        out.close();
        return out.good() ? scriptHost : std::string();
    }

    constexpr const char* kDataScriptMarker  = "<!--POLYPHASE_DATA_SCRIPT-->";
    constexpr const char* kRelayScriptMarker = "<!--POLYPHASE_RELAY_SCRIPT-->";

    std::string TrimWs(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // Quote a URL for embedding in a JS string literal. Also drops control
    // characters outright -- a stray newline or "</script>" in a profile
    // setting would otherwise break out of the tag.
    std::string JsQuote(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\')       { out += '\\'; out += c; }
            else if (c == '<')                 out += "\\x3C";
            else if ((unsigned char)c < 0x20)  continue;
            else                               out += c;
        }
        out += '"';
        return out;
    }

    // Build the <script> that publishes the relay URL to the page, or "" when
    // no relay is configured (marker just disappears and networking stays off).
    std::string BuildRelayScript(const PolyphaseBuildContext* ctx)
    {
        std::string url = TrimWs(ReadOption(ctx, kRelayUrlKey, ""));
        if (url.empty()) return "";

        if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0)
        {
            if (ctx->Log)
            {
                std::string msg = "WebGPU: relay URL '" + url +
                    "' is not ws:// or wss:// — ignoring it, networking will be off.";
                ctx->Log(POLYPHASE_BT_LOG_WARNING, msg.c_str());
            }
            return "";
        }

        return "<script>window.POLYPHASE_RELAY_URL = " + JsQuote(url) + ";</script>";
    }

    int32_t Web_PostPackage(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr ||
            ctx->projectName == nullptr || ctx->projectDir == nullptr)
            return 0;

        const std::string outDir = ctx->packageOutputDir;
        const std::string name   = ctx->projectName;
        const std::string built  = std::string(ctx->projectDir) + "/Build/WebGPU";

        // 1) The engine copied <name>.html; bring the .js/.wasm siblings.
        if (!CopyFileOverwrite(ctx, built + "/" + name + ".js",   outDir + "/" + name + ".js"))   return 0;
        if (!CopyFileOverwrite(ctx, built + "/" + name + ".wasm", outDir + "/" + name + ".wasm")) return 0;

        // 2) Pack the cooked content (Content.pak or loose tree) into a MEMFS
        //    preload bundle.
        const std::vector<std::string> entries = GatherPreloadEntries(outDir);
        if (entries.empty())
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                "WebGPU: nothing to preload in the package dir — build looks incomplete.");
            return 0;
        }

        const Toolchain tc = ResolveToolchain(ctx);
        const std::string scriptHost = WritePackageScript(ctx, tc, entries);
        if (scriptHost.empty())
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_ERROR,
                "WebGPU: could not write Intermediate/WebGPU/polyphase_package script.");
            return 0;
        }

        std::string cmd;
#if defined(_WIN32)
        if (tc == Toolchain::EmsdkNative)
        {
            cmd = "\"" + scriptHost + "\"";
        }
        else
        {
            std::string distro = ReadOption(ctx, kWslDistroKey, "");
            std::string wsl = distro.empty() ? "wsl " : ("wsl -d " + distro + " ");
            cmd = wsl + "bash -lc \"sh " + Sq(WinToWslPath(scriptHost)) + "\"";
        }
#else
        cmd = "bash " + Sq(scriptHost);
#endif

        if (ctx->WriteOutputLine) ctx->WriteOutputLine(cmd.c_str());
        const int rc = std::system(cmd.c_str());
        if (rc != 0)
        {
            if (ctx->Log)
            {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "WebGPU: file_packager failed (rc=%d). Check that the emsdk path "
                    "is correct and python is available in the build shell.", rc);
                ctx->Log(POLYPHASE_BT_LOG_ERROR, msg);
            }
            return 0;
        }

        // 3) index.html = the emcc shell output with the data-loader script
        //    injected ahead of the module script (shell.html carries the
        //    marker). Patch both the canonical <name>.html and index.html so
        //    either entry point works.
        const std::string htmlPath = outDir + "/" + name + ".html";
        const std::string dataTag = "<script src=\"" + name + ".data.js\"></script>";
        if (!PatchFileMarker(htmlPath, kDataScriptMarker, dataTag))
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_WARNING,
                "WebGPU: data-script marker not found in the emcc shell output — "
                "was Makefile_WebGPU built with the addon's shell.html?");
        }

        //    Multiplayer relay URL from the profile option. An empty setting
        //    removes the marker, leaving NET_IsActive() false. A missing marker
        //    only matters when a relay was actually requested.
        const std::string relayTag = BuildRelayScript(ctx);
        if (!PatchFileMarker(htmlPath, kRelayScriptMarker, relayTag) && !relayTag.empty())
        {
            if (ctx->Log) ctx->Log(POLYPHASE_BT_LOG_WARNING,
                "WebGPU: relay-script marker not found in the emcc shell output — "
                "a Network Relay URL is set but could not be injected. Rebuild "
                "with the addon's shell.html.");
        }
        else if (!relayTag.empty() && ctx->Log)
        {
            std::string msg = "WebGPU: multiplayer relay -> " + TrimWs(ReadOption(ctx, kRelayUrlKey, ""));
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, msg.c_str());
        }
        if (!CopyFileOverwrite(ctx, outDir + "/" + name + ".html", outDir + "/index.html"))
            return 0;

        if (ctx->Log)
        {
            char ok[512];
            std::snprintf(ok, sizeof(ok),
                "WebGPU package complete: %s/index.html (+ %s.js/.wasm/.data). "
                "Serve the folder from any static host.",
                outDir.c_str(), name.c_str());
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
        }
        return 1;
    }

    // ----- Run --------------------------------------------------------------

    // Write a serve script that hosts the packaged dir on localhost and opens
    // the default browser. Returns the HOST path (empty on failure).
    std::string WriteServeScript(const PolyphaseBuildContext* ctx, Toolchain tc, int port)
    {
        const std::string projectDir  = ctx->projectDir ? ctx->projectDir : "";
        const std::string packagedDir = ctx->packageOutputDir;
        const std::string intHost     = projectDir + "/Intermediate/WebGPU";
        std::error_code ec;
        std::filesystem::create_directories(intHost, ec);
        if (ec) return "";

        const std::string emsdk = ResolveEmsdkPath(ctx, tc);

#if defined(_WIN32)
        const std::string scriptHost = intHost + "/polyphase_serve.bat";
        std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return "";
        out << "@echo off\r\n";
        out << "setlocal\r\n";
        if (tc == Toolchain::EmsdkNative)
        {
            out << "call \"" << emsdk << "\\emsdk_env.bat\" >nul 2>&1\r\n";
            out << "if not defined EMSDK_PYTHON set EMSDK_PYTHON=python\r\n";
            // Detached, minimised server window; survives this script exiting.
            // A second Build & Run reuses the already-running server (the new
            // one exits on the port clash, which is fine).
            out << "start \"Polyphase WebGPU Server\" /min cmd /c \"cd /d \""
                << packagedDir << "\" && \"%EMSDK_PYTHON%\" -m http.server " << port << "\"\r\n";
        }
        else
        {
            std::string distro = ReadOption(ctx, kWslDistroKey, "");
            std::string wsl = distro.empty() ? "wsl" : ("wsl -d " + distro);
            // Kill a stale server on the port first, then host from WSL —
            // localhost forwarding makes it reachable from Windows.
            out << "start \"Polyphase WebGPU Server\" /min " << wsl
                << " bash -lc \"fuser -k " << port << "/tcp 2>/dev/null; cd "
                << Sq(WinToWslPath(packagedDir)) << " && exec python3 -m http.server "
                << port << "\"\r\n";
        }
        out << "timeout /t 2 /nobreak >nul\r\n";
        out << "start \"\" \"http://localhost:" << port << "/index.html\"\r\n";
        out << "exit /b 0\r\n";
        out.close();
        return out.good() ? scriptHost : std::string();
#else
        (void)tc;
        const std::string scriptHost = intHost + "/polyphase_serve.sh";
        std::ofstream out(scriptHost, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return "";
        out << "#!/bin/sh\n";
        out << "fuser -k " << port << "/tcp 2>/dev/null || true\n";
        out << "cd " << Sq(packagedDir) << "\n";
        out << "(python3 -m http.server " << port << " >/dev/null 2>&1 &)\n";
        out << "sleep 1\n";
        out << "(xdg-open \"http://localhost:" << port << "/index.html\" 2>/dev/null || "
               "open \"http://localhost:" << port << "/index.html\") &\n";
        out.close();
        return out.good() ? scriptHost : std::string();
#endif
    }

    int32_t Web_RunInEmulator(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        const Toolchain tc = ResolveToolchain(ctx);
        const int port = ReadPort(ctx);
        const std::string scriptHost = WriteServeScript(ctx, tc, port);
        if (scriptHost.empty())
        {
            std::snprintf(outCmd, cap,
                "echo \"WebGPU: could not write the serve script.\" && exit 1");
            return 1;
        }

#if defined(_WIN32)
        // Extra outer quotes: cmd.exe /c strips the outer pair when a command
        // starts with a quoted token.
        std::snprintf(outCmd, cap, "\"\"%s\"\"", scriptHost.c_str());
#else
        std::snprintf(outCmd, cap, "bash '%s'", scriptHost.c_str());
#endif
        return 1;
    }

    // ----- Editor profile UI ------------------------------------------------

    void Web_DrawProfileOptions(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->SetProfileSetting == nullptr) return;

        // NOTE: deliberately no polyphase.hideContentPak here — a Content.pak
        // is exactly what a web build wants (one packed archive inside the
        // MEMFS bundle instead of hundreds of loose preload entries).

        // ----- Toolchain ---------------------------------------------------
        static const char* kToolchains[]      = { "emsdk-native", "emsdk-wsl" };
        static const char* kToolchainLabels[] = { "Emscripten SDK (native)", "Emscripten SDK (WSL)" };
        std::string tcId = ReadOption(ctx, kToolchainKey, "emsdk-native");
        int tcIdx = (tcId == "emsdk-wsl") ? 1 : 0;
        if (ImGui::Combo("Toolchain", &tcIdx, kToolchainLabels, IM_ARRAYSIZE(kToolchainLabels)))
            ctx->SetProfileSetting(kToolchainKey, kToolchains[tcIdx]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "How the build runs:\n"
                "  Native - emsdk installed on this OS. Windows also needs a GNU make\n"
                "           (devkitPro MSYS2 / MSYS2 are auto-detected).\n"
                "  WSL    - emsdk installed inside WSL (wsl bash).");

        const bool wslMode = (tcIdx == 1);

        // ----- Emsdk path --------------------------------------------------
        {
            std::string cur = ReadOption(ctx, kEmsdkPathKey, "");
            char buf[256] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Emsdk Path", buf, sizeof(buf)))
                ctx->SetProfileSetting(kEmsdkPathKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Root of the emsdk install AS SEEN BY THE BUILD SHELL.\n"
                    "Native: leave empty to use %%EMSDK%% / C:\\emsdk / C:\\devkitPro\\emsdk.\n"
                    "WSL: leave empty for $HOME/emsdk.");
        }

#if defined(_WIN32)
        if (!wslMode)
        {
            std::string cur = ReadOption(ctx, kMakePathKey, "");
            char buf[256] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Make Path", buf, sizeof(buf)))
                ctx->SetProfileSetting(kMakePathKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "GNU make executable for the native route. Leave empty to probe\n"
                    "C:\\devkitPro\\msys2\\usr\\bin\\make.exe and C:\\msys64\\usr\\bin\\make.exe.");
        }
#endif

        if (wslMode)
        {
            std::string cur = ReadOption(ctx, kWslDistroKey, "");
            char buf[64] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("WSL Distro", buf, sizeof(buf)))
                ctx->SetProfileSetting(kWslDistroKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pass to `wsl -d <name>`. Leave empty for the default distro.");
        }

        // ----- Optimization ------------------------------------------------
        static const char* kOpts[]      = { "O0", "O2", "O3", "Oz" };
        static const char* kOptLabels[] = { "O0 (debug)", "O2 (default)", "O3 (speed)", "Oz (size)" };
        std::string opt = ReadOption(ctx, kOptKey, kOptDefault);
        int optIdx = 1;
        for (int i = 0; i < 4; ++i) if (opt == kOpts[i]) { optIdx = i; break; }
        if (ImGui::Combo("Optimization", &optIdx, kOptLabels, IM_ARRAYSIZE(kOptLabels)))
            ctx->SetProfileSetting(kOptKey, kOpts[optIdx]);

        // ----- Initial memory ----------------------------------------------
        {
            int mb = std::atoi(ReadOption(ctx, kMemoryKey, kMemoryDefault).c_str());
            if (mb < 64 || mb > 2048) mb = 256;
            if (ImGui::SliderInt("Initial Memory (MB)", &mb, 64, 1024))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", mb);
                ctx->SetProfileSetting(kMemoryKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("-sINITIAL_MEMORY. Growth is enabled, so this is a floor, not a cap.");
        }

        // ----- Parallel jobs ----------------------------------------------
        {
            std::string cur = ReadOption(ctx, kJobsKey, kJobsDefault);
            int jobs = std::atoi(cur.c_str());
            if (jobs < 1 || jobs > 64) jobs = 8;
            if (ImGui::SliderInt("Parallel Jobs", &jobs, 1, 32))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", jobs);
                ctx->SetProfileSetting(kJobsKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("`make -j<N>`. emcc TUs are light; 8 is a safe default.");
        }

        // ----- Server port -------------------------------------------------
        {
            int port = std::atoi(ReadOption(ctx, kPortKey, kPortDefault).c_str());
            if (port < 1024 || port > 65535) port = 6932;
            if (ImGui::InputInt("Server Port", &port))
            {
                if (port < 1024) port = 1024;
                if (port > 65535) port = 65535;
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", port);
                ctx->SetProfileSetting(kPortKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Build & Run hosts Packaged/web.webgpu on http://localhost:<port>/.");
        }

        // ----- Multiplayer relay -------------------------------------------
        {
            std::string cur = ReadOption(ctx, kRelayUrlKey, "");
            char buf[512] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputTextWithHint("Network Relay URL", "ws://localhost:5150 (empty = networking off)",
                                         buf, sizeof(buf)))
                ctx->SetProfileSetting(kRelayUrlKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Browsers have no UDP, so multiplayer tunnels through the relay in\n"
                    "Tools/relay/ (run `npm start` there). Injected into the packaged\n"
                    "index.html. Leave empty and Network.* is inactive.\n"
                    "HTTP does not need this. A page served over https:// needs wss://.");

            std::string trimmed = TrimWs(cur);
            if (!trimmed.empty() && trimmed.rfind("ws://", 0) != 0 && trimmed.rfind("wss://", 0) != 0)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "Relay URL must start with ws:// or wss:// - it will be ignored.");
        }

        // ----- Makefile ----------------------------------------------------
        {
            std::string cur = ReadOption(ctx, kMakefileKey, kMakefileDefault);
            char buf[256] = {0};
            std::strncpy(buf, cur.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Makefile", buf, sizeof(buf)))
                ctx->SetProfileSetting(kMakefileKey, buf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("emcc build makefile. Bare name resolves inside the addon (default: Makefile_WebGPU); absolute paths point at a fork.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Single-threaded wasm - runs from any static host (no COOP/COEP headers).");
        ImGui::TextDisabled("Recommended profile: Static Content ON + Content Pak ON, Embedded OFF.");
        ImGui::TextDisabled("Needs a WebGPU browser: Chrome/Edge 113+, Safari 18+, Firefox 141+.");
        ImGui::TextDisabled("First build downloads Dawn's emdawnwebgpu package (network required once).");
    }

    // Canonical descriptor. Strings are deep-copied by the registry; this
    // static instance just needs to outlive the RegisterBuildTarget call.
    static PolyphaseBuildTargetDesc gWebTarget{};
}
#endif // EDITOR

// ----- Plugin lifecycle -----------------------------------------------------

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    if (api) api->LogDebug("com.polyphase.build.target.webgpu loaded.");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI) sEngineAPI->LogDebug("com.polyphase.build.target.webgpu unloaded.");
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* /*nodeFactory*/) {}
static void RegisterScriptFuncs(lua_State* L) { (void)L; }

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr) return;

    if (hooks->RegisterBuildTarget == nullptr)
    {
        if (sEngineAPI)
        {
            sEngineAPI->LogWarning("com.polyphase.build.target.webgpu: this engine "
                                   "build predates the build-target API (need plugin "
                                   "apiVersion >= 4). Target not registered.");
        }
        return;
    }

    gWebTarget = {};
    gWebTarget.apiVersion            = POLYPHASE_BUILD_TARGET_API_VERSION;
    gWebTarget.targetId              = "web.webgpu";
    gWebTarget.displayName           = "Web (WebGPU)";
    gWebTarget.iconText              = "";
    gWebTarget.category              = "Web";
    gWebTarget.basePlatform          = 1; /* Platform::Linux — Unix-like cook, standard byte order */
    gWebTarget.binaryExtension       = ".html";
    gWebTarget.requiresDocker        = 0;
    gWebTarget.supportsRunOnDevice   = 0;
    gWebTarget.supportsEmulator      = 1; /* "emulator" = local HTTP server + default browser */
    gWebTarget.Validate              = &Web_Validate;
    gWebTarget.PreCook               = nullptr;
    gWebTarget.CookAsset             = nullptr; // Linux cook is correct — WebGPU eats RGBA8 + standard vertex layouts
    gWebTarget.GetCompileCommand     = &Web_GetCompileCommand;
    gWebTarget.GetCompiledBinaryPath = &Web_GetCompiledBinaryPath;
    gWebTarget.PostPackage           = &Web_PostPackage;
    gWebTarget.RunOnDevice           = nullptr;
    gWebTarget.RunInEmulator         = &Web_RunInEmulator;
    gWebTarget.DrawProfileOptions    = &Web_DrawProfileOptions;
    gWebTarget.SerializeProfileOptions   = nullptr;
    gWebTarget.DeserializeProfileOptions = nullptr;

    // Variant 2: ship an engine runtime. ActionManager writes
    // Generated/PolyphasePlatform_*.h bridges that #include the addon's
    // *_Platform.h headers, and Makefile_WebGPU sets -DPOLYPHASE_PLATFORM_ADDON=1
    // + -I<Generated/> so the engine's fork headers pick up the Web typedefs.
    gWebTarget.platformExtensionDir = "Runtime/Web";

    hooks->RegisterBuildTarget(hookId, &gWebTarget);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    if (desc == nullptr) return 1;
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.build.target.webgpu";
    desc->pluginVersion       = "1.0.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = nullptr;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
