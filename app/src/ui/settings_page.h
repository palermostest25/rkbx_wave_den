// Settings: touch-sized switches, segmented choices and dropdowns, one line of help each, in pages down
// the left: Display, Waveform, Deck info, Browser, Rekordbox, System, About.
#pragma once

#include <QWidget>

#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace wave {

class MainWindow;
class Segmented;
class Switch;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(MainWindow *window);
    void refresh();                          // show what is true now: some choices change elsewhere too
    void refreshScreens();
    void setStatus(const QString &text);
    void zoomChanged(double beats);
    void openPage(const QString &name);

private:
    void build();
    MainWindow *window_;
    QListWidget *menu_;
    QStackedWidget *stack_;
    QComboBox *screens_ = nullptr, *zoom_ = nullptr, *order_ = nullptr;
    Switch *fullscreen_ = nullptr;
    Segmented *count_ = nullptr, *time_ = nullptr, *browser_ = nullptr, *touchMode_ = nullptr;
    QLabel *status_ = nullptr, *installed_ = nullptr, *controller_ = nullptr, *installText_ = nullptr;
    QPushButton *install_ = nullptr;
    QProgressBar *progress_ = nullptr;
    std::vector<std::function<void()>> refreshers_;  // per control: read the setting again
};

}  // namespace wave
