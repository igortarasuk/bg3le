#include "console.h"

#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "debug_server.h"
#include "log.h"

extern char** environ;

namespace bg3le {
namespace {

std::atomic<bool> g_opened{false};

std::string dirname_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool executable(const std::string& path) {
    return ::access(path.c_str(), X_OK) == 0;
}

// Resolve a bare command against PATH.
bool on_path(const std::string& cmd) {
    if (cmd.find('/') != std::string::npos) return executable(cmd);
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string remaining(path);
    while (!remaining.empty()) {
        const std::size_t colon = remaining.find(':');
        const std::string dir = remaining.substr(0, colon);
        if (!dir.empty() && executable(dir + "/" + cmd)) return true;
        if (colon == std::string::npos) break;
        remaining.erase(0, colon + 1);
    }
    return false;
}

// Our own .so lives in <root>/build/, and the client submodule in
// <root>/client/.
std::string client_path() {
    const char* override_path = std::getenv("BG3LE_CLIENT");
    if (override_path != nullptr) return override_path;

    Dl_info info{};
    if (::dladdr(reinterpret_cast<void*>(&client_path), &info) == 0 ||
        info.dli_fname == nullptr) {
        return {};
    }
    const std::string root = dirname_of(dirname_of(info.dli_fname));
    return root + "/client/bg3lua";
}

bool console_enabled(const std::string& exe_dir) {
    if (const char* env = std::getenv("BG3LE_CONSOLE")) return env[0] == '1';

    // Parity with the Windows extender's setting, read from the same
    // filename next to the game binary.
    const std::string settings = exe_dir + "/ScriptExtenderSettings.json";
    std::FILE* f = std::fopen(settings.c_str(), "rb");
    if (f == nullptr) return false;
    std::string text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    const std::size_t key = text.find("\"CreateConsole\"");
    if (key == std::string::npos) return false;
    const std::size_t colon = text.find(':', key);
    if (colon == std::string::npos) return false;
    const std::size_t value = text.find_first_not_of(" \t\r\n", colon + 1);
    return value != std::string::npos && text.compare(value, 4, "true") == 0;
}

// Inside the Steam runtime container the host's terminals are neither on
// PATH nor loadable (their libraries are not visible), but
// steam-runtime-launch-client can run a command back on the host.
std::string host_path_of(const std::string& cmd) {
    if (!on_path("steam-runtime-launch-client")) return {};
    for (const char* dir : {"/usr/bin/", "/usr/local/bin/", "/bin/"}) {
        const std::string host = std::string("/run/host") + dir + cmd;
        if (executable(host)) return std::string(dir) + cmd;
    }
    return {};
}

struct Launch {
    std::string command;
    bool via_host = false;
};

// $TERMINAL, then a guess from the desktop, then xterm -- each tried inside
// the container first, then on the host.
Launch pick_terminal() {
    std::vector<std::string> candidates;

    if (const char* env = std::getenv("TERMINAL")) {
        if (env[0] != '\0') candidates.emplace_back(env);
    }

    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP")) {
        const std::string d(desktop);
        const struct { const char* match; const char* term; } table[] = {
            {"KDE", "konsole"},         {"GNOME", "gnome-terminal"},
            {"XFCE", "xfce4-terminal"}, {"MATE", "mate-terminal"},
            {"LXQt", "qterminal"},      {"Cinnamon", "gnome-terminal"},
            {"Deepin", "deepin-terminal"}, {"Hyprland", "foot"},
            {"sway", "foot"},
        };
        for (const auto& entry : table) {
            if (d.find(entry.match) != std::string::npos) {
                candidates.emplace_back(entry.term);
            }
        }
    }

    candidates.emplace_back("xterm");

    for (const std::string& candidate : candidates) {
        if (on_path(candidate)) return {candidate, false};
        const std::string host = host_path_of(candidate);
        if (!host.empty()) return {host, true};
    }
    return {"xterm", false};
}

// Terminals disagree on how a command is passed.
std::vector<std::string> build_argv(const std::string& term,
                                    const std::string& client) {
    const std::size_t slash = term.find_last_of('/');
    const std::string name = slash == std::string::npos ? term : term.substr(slash + 1);

    if (name == "gnome-terminal" || name == "tilix" || name == "ptyxis") {
        return {term, "--", client};
    }
    if (name == "xfce4-terminal" || name == "mate-terminal" || name == "terminator") {
        return {term, "-e", client};  // these take a single command string
    }
    if (name == "kitty" || name == "foot") {
        return {term, client};
    }
    if (name == "wezterm") {
        return {term, "start", "--", client};
    }
    return {term, "-e", client};  // xterm, konsole, alacritty, urxvt, st, ...
}

}  // namespace

void maybe_open_console() {
    bool expected = false;
    if (!g_opened.compare_exchange_strong(expected, true)) return;

    char exe[4096];
    const ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return;
    exe[len] = '\0';

    if (!console_enabled(dirname_of(exe))) {
        g_opened.store(false);
        return;
    }

    const std::string client = client_path();
    if (client.empty() || !executable(client)) {
        statusf("CreateConsole: client not found at '%s'", client.c_str());
        return;
    }

    const Launch launch = pick_terminal();
    if (!launch.via_host && !on_path(launch.command)) {
        statusf("CreateConsole: no usable terminal (tried '%s')",
                launch.command.c_str());
        return;
    }

    std::vector<std::string> args = build_argv(launch.command, client);
    if (launch.via_host) {
        // launch-client waits for the host command, and pressure-vessel in
        // turn waits for launch-client -- so without detaching, the game's
        // container cannot exit until the console window is closed.
        std::vector<std::string> prefix{"steam-runtime-launch-client", "--host", "--"};
        if (executable("/run/host/usr/bin/setsid")) {
            prefix.emplace_back("/usr/bin/setsid");
            prefix.emplace_back("-f");
        } else {
            statusf("CreateConsole: setsid missing on host; the game will not "
                    "exit until the console is closed");
        }
        args.insert(args.begin(), prefix.begin(), prefix.end());
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    const int rc = ::posix_spawnp(&pid, argv[0], nullptr, &attr, argv.data(), environ);
    ::posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        statusf("CreateConsole: failed to launch %s (%s)", launch.command.c_str(),
                std::strerror(rc));
        return;
    }

    // Reap it ourselves rather than touching the game's SIGCHLD handling.
    std::thread([pid] { int status = 0; ::waitpid(pid, &status, 0); }).detach();
    statusf("CreateConsole: opened %s%s running %s", launch.command.c_str(),
            launch.via_host ? " (on host)" : "", client.c_str());
}

}  // namespace bg3le
