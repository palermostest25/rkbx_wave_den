#include "app/install.h"

#include "app/updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QProcess>
#include <QTemporaryFile>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

Q_LOGGING_CATEGORY(lcInstall, "wave.install")

namespace wave::install {
namespace {
const char *kName = "Rekordbox Wave";

#ifdef Q_OS_WIN
const wchar_t *kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RekordboxWave";

QString known(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    QString out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    CoTaskMemFree(path);
    return out;
}

QStringList shortcuts() {
    return {known(FOLDERID_CommonPrograms) + "/Rekordbox Wave.lnk", known(FOLDERID_PublicDesktop) + "/Rekordbox Wave.lnk"};
}

bool shortcut(const QString &link, const QString &exe) {
    IShellLinkW *shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&shell))))
        return false;
    const std::wstring path = QDir::toNativeSeparators(exe).toStdWString();
    const std::wstring dir = QDir::toNativeSeparators(QFileInfo(exe).absolutePath()).toStdWString();
    shell->SetPath(path.c_str());
    shell->SetWorkingDirectory(dir.c_str());
    shell->SetIconLocation(path.c_str(), 0);
    shell->SetDescription(L"Rekordbox Wave: the deck screen for Rekordbox");
    IPersistFile *file = nullptr;
    bool ok = false;
    if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
        QDir().mkpath(QFileInfo(link).absolutePath());
        ok = SUCCEEDED(file->Save(QDir::toNativeSeparators(link).toStdWString().c_str(), TRUE));
        file->Release();
    }
    shell->Release();
    return ok;
}

void setString(HKEY key, const wchar_t *name, const QString &value) {
    const std::wstring v = value.toStdWString();
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void setNumber(HKEY key, const wchar_t *name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof value);
}

// A copy that survives the copy's own folder being replaced: rename what is there, then put the new one in.
bool putInPlace(const QString &from, const QString &to) {
    const QString old = QFileInfo(to).absoluteDir().filePath(QFileInfo(to).completeBaseName() + ".old.exe");
    QFile::remove(old);
    if (QFileInfo::exists(to) && !QFile::rename(to, old)) return false;
    if (!QFile::copy(from, to)) {
        QFile::rename(old, to);
        return false;
    }
    return true;
}
#endif
}  // namespace

QString folder() {
#ifdef Q_OS_WIN
    return known(FOLDERID_ProgramFilesX64) + "/" + kName;
#else
    return {};
#endif
}

QString target() { return folder() + "/Rekordbox Wave.exe"; }

bool isInstalled() {
#ifdef Q_OS_WIN
    const QString here = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath()).toLower();
    const QString programs = (known(FOLDERID_ProgramFilesX64) + "/").toLower();
    const QString user = (known(FOLDERID_UserProgramFiles) + "/").toLower();  // where the Setup download installs
    return here.startsWith(programs) || (!user.isEmpty() && here.startsWith(user));
#else
    return true;
#endif
}

bool offered() {
#if defined(Q_OS_WIN) && defined(WAVE_PACKAGED)
    return !isInstalled();
#else
    return false;
#endif
}

bool writable(const QString &dir) {
    QTemporaryFile probe(QDir(dir).filePath("wave-write-XXXXXX"));
    return probe.open();
}

QString elevated(const QString &exe, const QStringList &arguments) {
#ifdef Q_OS_WIN
    QStringList quoted;
    for (const QString &a : arguments) quoted << '"' + a + '"';
    const std::wstring file = QDir::toNativeSeparators(exe).toStdWString(), params = quoted.join(' ').toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess)
        return GetLastError() == ERROR_CANCELLED ? QStringLiteral("Windows' permission was not given") : QStringLiteral("Windows did not start it");
    WaitForSingleObject(info.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0 ? QString() : QStringLiteral("It did not finish (code %1)").arg(code);
#else
    Q_UNUSED(exe);
    Q_UNUSED(arguments);
    return QStringLiteral("Installing is for Windows");
#endif
}

QString installHere() {
#ifdef Q_OS_WIN
    const QString why = elevated(QCoreApplication::applicationFilePath(), {"--install"});
    if (!why.isEmpty()) return why;
    if (!QFileInfo::exists(target())) return QStringLiteral("Wave was not installed");
    qCInfo(lcInstall) << "Installed in" << folder();
    return {};
#else
    return QStringLiteral("Installing is for Windows");
#endif
}

int runInstall() {
#ifdef Q_OS_WIN
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QDir().mkpath(folder());
    if (!putInPlace(QCoreApplication::applicationFilePath(), target())) return 2;
    for (const QString &link : shortcuts()) shortcut(link, target());
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const QString exe = QDir::toNativeSeparators(target());
        setString(key, L"DisplayName", kName);
        setString(key, L"DisplayVersion", version());
        setString(key, L"Publisher", QStringLiteral("Rekordbox Wave contributors"));
        setString(key, L"DisplayIcon", exe);
        setString(key, L"InstallLocation", QDir::toNativeSeparators(folder()));
        setString(key, L"UninstallString", '"' + exe + "\" --uninstall");
        setString(key, L"URLInfoAbout", QStringLiteral("https://github.com/palermostest25/rkbx_wave_den"));
        setNumber(key, L"NoModify", 1);
        setNumber(key, L"NoRepair", 1);
        setNumber(key, L"EstimatedSize", DWORD(QFileInfo(target()).size() / 1024));
        RegCloseKey(key);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    CoUninitialize();
    return 0;
#else
    return 1;
#endif
}

int runReplace(const QString &exe) {
#ifdef Q_OS_WIN
    // The Wave being updated may still be closing: a running exe can be renamed, and is.
    for (int i = 0; i < 20; ++i) {
        if (putInPlace(QCoreApplication::applicationFilePath(), QDir::fromNativeSeparators(exe))) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                setString(key, L"DisplayVersion", version());
                RegCloseKey(key);
            }
            return 0;
        }
        QThread::msleep(250);
    }
    return 3;
#else
    Q_UNUSED(exe);
    return 1;
#endif
}

int runUninstall(bool ask) {
#ifdef Q_OS_WIN
    if (ask && QMessageBox::question(nullptr, QStringLiteral("Rekordbox Wave"),
                              QStringLiteral("Remove Rekordbox Wave from this computer?\n\nYour settings and Rekordbox stay as they are.")) !=
        QMessageBox::Yes)
        return 1;
    const QString here = QCoreApplication::applicationFilePath();
    if (ask && !writable(QCoreApplication::applicationDirPath())) {
        // Removing from Program Files needs Windows' permission: ask, then let the elevated copy do it.
        const QString why = elevated(here, {"--uninstall-now"});
        if (!why.isEmpty()) QMessageBox::warning(nullptr, QStringLiteral("Rekordbox Wave"), QStringLiteral("Wave was not removed: %1").arg(why));
        return why.isEmpty() ? 0 : 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (const QString &link : shortcuts()) QFile::remove(link);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    // Wave's own folder goes once this process has ended.
    // cmd.exe reads its own quoting, not the C runtime's: its command line goes to it as written.
    const QString dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    QProcess cleanup;
    cleanup.setProgram(qEnvironmentVariable("ComSpec", "C:\\Windows\\System32\\cmd.exe"));
    cleanup.setNativeArguments(QStringLiteral("/d /c \"ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%1\"\"").arg(dir));
    cleanup.setWorkingDirectory(QDir::tempPath());  // not inside the folder it removes
    cleanup.startDetached();
    return 0;
#else
    Q_UNUSED(ask);
    return 1;
#endif
}

}  // namespace wave::install
