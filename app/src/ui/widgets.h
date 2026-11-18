// Wave's controls, made for fingers on a booth screen: a phone-style switch, segmented choices, dropdowns
// styled like the rest of Wave, and the stylesheet that gives everything one look.
#pragma once

#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QPushButton>
#include <QVariant>
#include <QVariantAnimation>

#include <vector>

namespace wave {

constexpr const char *kAccent = "#1a79eb";

QString styleSheet(double settingsScale);
QPushButton *button(const QString &title, QWidget *parent = nullptr);
void mark(QWidget *widget, const char *property, const QVariant &value);  // set a styled property and restyle

class Switch : public QAbstractButton {
    Q_OBJECT
public:
    explicit Switch(bool checked = false, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {62, 36}; }
    void setQuietly(bool on);      // as when Wave updates it: no animation, no signal

protected:
    void paintEvent(QPaintEvent *) override;

private:
    double knob_ = 0;
    QVariantAnimation slide_;
};

// A row of choices, one lit: for two to five options, quicker than a dropdown on a touch screen.
class Segmented : public QFrame {
    Q_OBJECT
public:
    Segmented(const std::vector<std::pair<QString, QVariant>> &options, const QVariant &value, QWidget *parent = nullptr);
    QVariant value() const;
    void setValue(const QVariant &value, bool notify = false);

signals:
    void changed(const QVariant &value);

private:
    std::vector<std::pair<QPushButton *, QVariant>> buttons_;
};

QComboBox *combo(const std::vector<std::pair<QString, QVariant>> &options = {}, const QVariant &value = {}, const QString &accessible = {});

}  // namespace wave
