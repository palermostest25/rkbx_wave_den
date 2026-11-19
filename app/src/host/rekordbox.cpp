#include "host/rekordbox.h"

#include "core/bridge.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <ctime>
#endif

namespace wave::host {
namespace {

#ifdef Q_OS_WIN
const QString kLibrarySuffix = QStringLiteral(".dll");
#else
const QString kLibrarySuffix = QStringLiteral(".dylib");
const QString kMacApp = QStringLiteral("/Applications/rekordbox 7/rekordbox.app");
const QString kMacExecutable = kMacApp + QStringLiteral("/Contents/MacOS/rekordbox");
#endif

QByteArray bundledBridge() {
    QFile file(QStringLiteral(":/bridge/wave-bridge") + kLibrarySuffix);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void pause(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

#ifdef Q_OS_WIN
QString wide(const wchar_t *text) { return QString::fromWCharArray(text); }

std::vector<HWND> topWindows(uint32_t pid) {
    struct Found { uint32_t pid; std::vector<HWND> windows; } found{pid, {}};
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        auto *f = reinterpret_cast<Found *>(data);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == f->pid && GetWindowTextLengthW(hwnd) && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER))
            f->windows.push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found.windows;
}

// The RVA of an exported function in a PE image, or 0.
uint32_t exportRva(const QByteArray &image, const char *name) {
    auto u16 = [&](qsizetype at) { return at + 2 <= image.size() ? uint16_t(uchar(image[at]) | uchar(image[at + 1]) << 8) : 0; };
    auto u32 = [&](qsizetype at) { return at + 4 <= image.size() ? uint32_t(u16(at) | uint32_t(u16(at + 2)) << 16) : 0; };
    if (!image.startsWith("MZ")) return 0;
    const uint32_t pe = u32(0x3c);
    if (image.mid(pe, 4) != QByteArray("PE\0\0", 4)) return 0;
    const int sections = u16(pe + 6), optionalSize = u16(pe + 20);
    const qsizetype optional = pe + 24;
    if (u16(optional) != 0x20b) return 0;  // 64-bit only
    const uint32_t exports = u32(optional + 112);
    auto offset = [&](uint32_t rva) -> qsizetype {
        for (int i = 0; i < sections; ++i) {
            const qsizetype at = optional + optionalSize + 40 * i;
            const uint32_t size = std::max(u32(at + 8), u32(at + 16)), address = u32(at + 12), raw = u32(at + 20);
            if (address <= rva && rva < address + size) return raw + (rva - address);
        }
        return -1;
    };
    const qsizetype dir = exports ? offset(exports) : -1;
    if (dir < 0) return 0;
    const uint32_t functions = u32(dir + 20), names = u32(dir + 24);
    const qsizetype functionsAt = offset(u32(dir + 28)), namesAt = offset(u32(dir + 32)), ordinalsAt = offset(u32(dir + 36));
    if (functionsAt < 0 || namesAt < 0 || ordinalsAt < 0) return 0;
    const QByteArray wanted = QByteArray(name) + '\0';
    for (uint32_t i = 0; i < names; ++i) {
        const qsizetype at = offset(u32(namesAt + 4 * i));
        if (at >= 0 && image.mid(at, wanted.size()) == wanted) {
            const uint16_t ordinal = u16(ordinalsAt + 2 * i);
            return ordinal < functions ? u32(functionsAt + 4 * ordinal) : 0;
        }
    }
    return 0;
}

uintptr_t moduleBase(uint32_t pid, const QString &library) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof entry;
    uintptr_t base = 0;
    const QString wanted = QDir::toNativeSeparators(QFileInfo(library).absoluteFilePath()).toLower();
    for (BOOL more = Module32FirstW(snap, &entry); more; more = Module32NextW(snap, &entry))
        if (wide(entry.szExePath).toLower() == wanted) { base = reinterpret_cast<uintptr_t>(entry.modBaseAddr); break; }
    CloseHandle(snap);
    return base;
}
#endif

}  // namespace

QString supportedText() { return QStringLiteral("%1.%2.%3").arg(kSupported[0]).arg(kSupported[1]).arg(kSupported[2]); }

QString Install::text() const {
    QStringList parts;
    for (int n : version) parts << QString::number(n);
    return parts.join('.');
}

bool Install::supported() const {
    return version.size() >= 3 && version[0] == kSupported[0] && version[1] == kSupported[1] && version[2] == kSupported[2];
}

QList<int> versionOf(const QString &text) {
    QList<int> numbers;
    for (const auto &m : QRegularExpression(QStringLiteral("\\d+")).globalMatch(text)) numbers << m.captured().toInt();
    return numbers;
}

std::vector<Install> installed(const QString &root) {
    std::vector<Install> found;
#ifdef Q_OS_WIN
    // Each version installs in a folder of its own: Program Files\rekordbox\rekordbox 7.2.19\rekordbox.exe.
    const QDir base(root.isEmpty() ? qEnvironmentVariable("ProgramFiles", "C:/Program Files") + "/rekordbox" : root);
    for (const QString &folder : base.entryList({"rekordbox *"}, QDir::Dirs)) {
        const QString exe = base.filePath(folder + "/rekordbox.exe");
        const QList<int> version = versionOf(folder);
        if (!version.isEmpty() && QFileInfo(exe).isFile()) found.push_back({version, exe});
    }
#elif defined(Q_OS_MACOS)
    const QString app = root.isEmpty() ? kMacApp : root;
    const QSettings info(app + "/Contents/Info.plist", QSettings::NativeFormat);
    const QList<int> version = versionOf(info.value("CFBundleShortVersionString").toString());
    if (!version.isEmpty()) found.push_back({version, app + "/Contents/MacOS/rekordbox"});
#else
    Q_UNUSED(root);
#endif
    std::sort(found.begin(), found.end(), [](const Install &a, const Install &b) { return a.version < b.version; });
    return found;
}

std::optional<Install> supportedInstall() {
    const auto all = installed();
    for (auto it = all.rbegin(); it != all.rend(); ++it) if (it->supported()) return *it;
    return std::nullopt;
}

QString summary() {
    const auto found = installed();
    if (found.empty()) return QStringLiteral("Rekordbox is not installed. Wave needs Rekordbox %1.").arg(supportedText());
    QStringList others;
    bool has = false;
    for (const Install &i : found) (i.supported() ? has = true : (others << i.text(), false));
    if (has)
        return QStringLiteral("Rekordbox %1 is installed").arg(supportedText()) +
               (others.isEmpty() ? QString() : QStringLiteral(" (also %1; Wave uses %2)").arg(others.join(", "), supportedText())) + '.';
#ifdef Q_OS_WIN
    return QStringLiteral("Rekordbox %1 is installed, but Wave needs %2. Install %2 beside it.").arg(others.join(", "), supportedText());
#else
    return QStringLiteral("Rekordbox %1 is installed, but Wave needs %2.").arg(found.back().text(), supportedText());
#endif
}

int stopUpdater() {
#ifdef Q_OS_WIN
    // Rekordbox's update manager, which offers a newer Rekordbox than the one Wave's bridge was checked against.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    int stopped = 0;
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"Upmgr rekordbox.exe") != 0) continue;
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID)) {
            if (TerminateProcess(process, 0)) ++stopped;
            CloseHandle(process);
        }
    }
    CloseHandle(snap);
    return stopped;
#else
    return 0;
#endif
}

uint32_t rekordboxPid() {
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    uint32_t pid = 0;
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry))
        if (_wcsicmp(entry.szExeFile, L"rekordbox.exe") == 0) { pid = entry.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
#elif defined(Q_OS_MACOS)
    std::vector<pid_t> pids(4096);
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), int(pids.size() * sizeof(pid_t)));
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const QByteArray wanted = kMacExecutable.toUtf8();
    for (int i = 0; i < bytes / int(sizeof(pid_t)); ++i)
        if (pids[i] > 0 && proc_pidpath(pids[i], path, sizeof path) > 0 && wanted == path) return uint32_t(pids[i]);
    return 0;
#else
    return 0;
#endif
}

std::optional<double> processAge(uint32_t pid) {
    if (!pid) return std::nullopt;
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::nullopt;
    FILETIME created, exited, kernel, user, nowTime;
    const BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (!ok) return std::nullopt;
    GetSystemTimeAsFileTime(&nowTime);
    auto ticks = [](const FILETIME &t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return std::max(0.0, double(ticks(nowTime) - ticks(created)) / 1e7);
#elif defined(Q_OS_MACOS)
    proc_bsdinfo info{};
    if (proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof info) != int(sizeof info)) return std::nullopt;
    timespec t{};
    clock_gettime(CLOCK_REALTIME, &t);
    return std::max(0.0, double(t.tv_sec - info.pbi_start_tvsec) + (t.tv_nsec / 1e9 - info.pbi_start_tvusec / 1e6));
#else
    return std::nullopt;
#endif
}

QString processExecutable(uint32_t pid) {
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2];
    DWORD size = DWORD(std::size(path));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    return ok ? QDir::fromNativeSeparators(QString::fromWCharArray(path, int(size))) : QString();
#elif defined(Q_OS_MACOS)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    return proc_pidpath(int(pid), path, sizeof path) > 0 ? QString::fromUtf8(path) : QString();
#else
    Q_UNUSED(pid);
    return {};
#endif
}

QStringList bridgeModules(uint32_t pid) {
    QStringList found;
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return found;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Module32FirstW(snap, &entry); more; more = Module32NextW(snap, &entry)) {
        const QString name = wide(entry.szModule);
        if (name.startsWith(QLatin1String("wave-bridge-"), Qt::CaseInsensitive)) found << name;
    }
    CloseHandle(snap);
#else
    Q_UNUSED(pid);
#endif
    return found;
}

QString bridgeName() {
    const QByteArray library = bundledBridge();
    return QStringLiteral("wave-bridge-%1%2")
        .arg(QString::fromLatin1(QCryptographicHash::hash(library, QCryptographicHash::Sha256).toHex().left(16)), kLibrarySuffix);
}

QString installBridge(QString *error) {
    const QByteArray library = bundledBridge();
    if (library.isEmpty()) {
        if (error) *error = QStringLiteral("This build of Wave has no Rekordbox bridge");
        return {};
    }
    const QString folder = bridge::defaultFolder();
    QDir().mkpath(folder);
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString path = QDir(folder).filePath(bridgeName());
    if (!QFileInfo::exists(path)) {
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(library) != library.size() || !out.commit()) {
            if (error) *error = QStringLiteral("The bridge could not be written: %1").arg(out.errorString());
            return {};
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    const QString config = QDir(folder).filePath("openssl.cnf");
    // Forward slashes: OpenSSL reads a backslash as an escape.
    const QByteArray text = QStringLiteral("openssl_conf = wave_init\n[wave_init]\nproviders = wave_providers\n"
                                           "[wave_providers]\ndefault = default_provider\nwave_bridge = wave_bridge_provider\n"
                                           "[default_provider]\nactivate = 1\n"
                                           "[wave_bridge_provider]\nmodule = %1\nactivate = 1\n")
                                .arg(QDir::fromNativeSeparators(path)).toUtf8();
    QFile existing(config);
    if (!(existing.open(QIODevice::ReadOnly) && existing.readAll() == text)) {
        existing.close();
        QSaveFile out(config);
        if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size() || !out.commit()) {
            if (error) *error = QStringLiteral("The bridge's config could not be written: %1").arg(out.errorString());
            return {};
        }
    }
    return config;
}

bool staleBridge(uint32_t pid) {
#ifdef Q_OS_WIN
    const QStringList loaded = bridgeModules(pid);
    return !loaded.isEmpty() && !loaded.contains(bridgeName(), Qt::CaseInsensitive);
#else
    Q_UNUSED(pid);
    return false;  // macOS can't list another process's libraries without more rights than Wave asks for
#endif
}

QString launch(bool hidden, const QString &executable) {
    if (rekordboxPid()) return QStringLiteral("Quit Rekordbox first; Wave then starts it with the bridge");
    QString error;
#ifdef Q_OS_WIN
    QString exe = executable;
    if (exe.isEmpty()) {
        const auto found = installed();
        const auto supported = supportedInstall();
        if (supported) exe = supported->executable;
        else if (!found.empty()) exe = found.back().executable;
    }
    if (exe.isEmpty()) return QStringLiteral("Rekordbox was not found in Program Files\\rekordbox");
    const QString config = installBridge(&error);
    if (config.isEmpty()) return error;
    // Rekordbox gets Wave's environment, plus its bridge config.
    QStringList environment;
    for (const QString &entry : QProcessEnvironment::systemEnvironment().toStringList())
        if (!entry.startsWith("OPENSSL_CONF=", Qt::CaseInsensitive) && !entry.startsWith("RKBX_WAVE_BRIDGE=")) environment << entry;
    environment << "OPENSSL_CONF=" + QDir::toNativeSeparators(config) << "RKBX_WAVE_BRIDGE=1";
    std::wstring block;
    for (const QString &entry : environment) { block += entry.toStdWString(); block += L'\0'; }
    block += L'\0';
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    if (hidden) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWMINNOACTIVE;  // minimised: Wave keeps the screen
    }
    PROCESS_INFORMATION info{};
    std::wstring command = L"\"" + QDir::toNativeSeparators(exe).toStdWString() + L"\"";
    const std::wstring folder = QDir::toNativeSeparators(QFileInfo(exe).absolutePath()).toStdWString();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, block.data(),
                        folder.c_str(), &startup, &info))
        return QStringLiteral("Rekordbox did not start (error %1)").arg(GetLastError());
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return {};
#elif defined(Q_OS_MACOS)
    Q_UNUSED(executable);
    if (!QFileInfo(kMacApp).isDir()) return QStringLiteral("Rekordbox was not found at %1").arg(kMacApp);
    const QString config = installBridge(&error);
    if (config.isEmpty()) return error;
    QStringList args;
    if (hidden) args << "-g" << "-j";
    args << "-a" << kMacApp << "--env" << "OPENSSL_CONF=" + config << "--env" << "RKBX_WAVE_BRIDGE=1";
    QProcess open;
    open.start("/usr/bin/open", args);
    if (!open.waitForFinished(15000) || open.exitCode() != 0)
        return QStringLiteral("Rekordbox did not start: %1").arg(QString::fromUtf8(open.readAllStandardError()).trimmed());
    return {};
#else
    Q_UNUSED(hidden);
    Q_UNUSED(executable);
    return QStringLiteral("Rekordbox runs on Windows and macOS");
#endif
}

void askToQuit(uint32_t pid) {
    if (!pid) return;
#ifdef Q_OS_WIN
    // As taskkill without /F, and as its own close button: Rekordbox asks first if a deck is playing.
    for (HWND hwnd : topWindows(pid)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
#elif defined(Q_OS_MACOS)
    QProcess::startDetached("/usr/bin/osascript", {"-e", "tell application id \"com.pioneerdj.rekordboxdj\" to quit"});
#endif
}

QString restart(bool hidden) {
    QString executable;
    if (const uint32_t pid = rekordboxPid()) {
        executable = processExecutable(pid);  // start the same install again, wherever it lives
#ifndef Q_OS_WIN
        executable.clear();
#endif
        askToQuit(pid);
        for (int i = 0; i < 60 && rekordboxPid(); ++i) pause(500);
        if (rekordboxPid()) return QStringLiteral("Rekordbox is still open; it may be waiting for you to confirm quitting");
    }
    return launch(hidden, executable);
}

QString attach(uint32_t pid, QString *message) {
#ifdef Q_OS_WIN
    QString error;
    if (installBridge(&error).isEmpty()) return error;
    const QString library = QDir(bridge::defaultFolder()).filePath(bridgeName());
    QFile file(library);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("The bridge could not be read");
    const uint32_t rva = exportRva(file.readAll(), "wave_bridge_attach");
    if (!rva) return QStringLiteral("This bridge has no wave_bridge_attach");
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process)
        return QStringLiteral("Wave may not open Rekordbox (error %1): is Rekordbox running as administrator?").arg(GetLastError());
    auto run = [&](LPTHREAD_START_ROUTINE start, void *argument, DWORD waitMs, DWORD *code) -> QString {
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, start, argument, 0, nullptr);
        if (!thread) return QStringLiteral("Rekordbox refused a thread (error %1)").arg(GetLastError());
        const DWORD waited = WaitForSingleObject(thread, waitMs);
        GetExitCodeThread(thread, code);
        CloseHandle(thread);
        return waited == WAIT_OBJECT_0 ? QString() : QStringLiteral("Rekordbox did not answer in time");
    };
    QString failure;
    const std::wstring path = QDir::toNativeSeparators(QFileInfo(library).absoluteFilePath()).toStdWString();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) failure = QStringLiteral("Rekordbox refused memory (error %1)").arg(GetLastError());
    DWORD code = 0;
    if (failure.isEmpty()) {
        SIZE_T written = 0;
        if (!WriteProcessMemory(process, remote, path.c_str(), bytes, &written) || written != bytes)
            failure = QStringLiteral("Wave could not hand Rekordbox the bridge");
        else {
            // kernel32 sits at the same address in every process of this boot.
            auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
            failure = run(loadLibrary, remote, 10000, &code);
            if (failure.isEmpty() && !code) failure = QStringLiteral("Rekordbox could not load the bridge");
        }
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    if (failure.isEmpty()) {
        const uintptr_t base = moduleBase(pid, library);
        if (!base) failure = QStringLiteral("The bridge did not stay loaded in Rekordbox");
        else failure = run(reinterpret_cast<LPTHREAD_START_ROUTINE>(base + rva), nullptr, 20000, &code);
    }
    CloseHandle(process);
    if (!failure.isEmpty()) return failure;
    static const char *answers[] = {"connected", "Rekordbox already runs a Wave bridge", "That process is not Rekordbox",
                                    "The bridge could not open Wave's state file",
                                    "This Rekordbox build is not supported by the bridge",
                                    "The bridge could not reach Rekordbox's UI thread"};
    const QString answer = code < 6 ? QString::fromLatin1(answers[code]) : QStringLiteral("The bridge answered %1").arg(code);
    if (message) *message = answer;
    return code == 0 ? QString() : answer;
#else
    Q_UNUSED(pid);
    Q_UNUSED(message);
    return QStringLiteral("Live connecting is for Windows");
#endif
}

QString show(bool maximize) {
#ifdef Q_OS_WIN
    const uint32_t pid = rekordboxPid();
    if (!pid) return QStringLiteral("Rekordbox is not running");
    std::vector<HWND> main, others;
    for (HWND hwnd : topWindows(pid)) {
        wchar_t title[32] = {};
        GetWindowTextW(hwnd, title, 32);
        (QString::fromWCharArray(title) == QLatin1String("rekordbox") ? main : others).push_back(hwnd);
    }
    // Only its main window grows; its helper windows and questions come after it, so a question lands on top.
    for (HWND hwnd : main) { ShowWindow(hwnd, maximize ? SW_MAXIMIZE : SW_RESTORE); SetForegroundWindow(hwnd); }
    for (HWND hwnd : others) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
    return {};
#elif defined(Q_OS_MACOS)
    Q_UNUSED(maximize);
    return QProcess::execute("/usr/bin/open", {"-a", kMacApp}) == 0 ? QString() : QStringLiteral("Rekordbox could not be shown");
#else
    Q_UNUSED(maximize);
    return QStringLiteral("Rekordbox runs on Windows and macOS");
#endif
}

void hide() {
#ifdef Q_OS_WIN
    if (const uint32_t pid = rekordboxPid())
        for (HWND hwnd : topWindows(pid)) ShowWindow(hwnd, SW_MINIMIZE);
#endif
}

}  // namespace wave::host
