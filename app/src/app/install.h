// The portable exe installs itself (Windows): into Program Files, with Start menu and desktop shortcuts and
// an entry in Windows' installed apps, so it can be removed from there. Windows asks once for permission.
#pragma once

#include <QString>
#include <QStringList>

namespace wave::install {

QString folder();                  // C:\Program Files\Rekordbox Wave
QString target();                  // ...\Rekordbox Wave.exe
bool isInstalled();                // this Wave runs from an installed copy (Program Files, or the installer's)
bool offered();                    // the portable exe on Windows: INSTALL is offered
bool writable(const QString &folder);

// Runs ``exe`` with ``arguments`` as administrator and waits for it: "" or why it didn't happen.
QString elevated(const QString &exe, const QStringList &arguments);

// From the UI: install this exe (asking Windows), then start the installed Wave. "" or what went wrong.
QString installHere();

// The elevated halves, run with --install, --replace <exe> and --uninstall (--uninstall-now: no question,
// already elevated). Return the process exit code.
int runInstall();
int runReplace(const QString &exe);
int runUninstall(bool ask);

}  // namespace wave::install
