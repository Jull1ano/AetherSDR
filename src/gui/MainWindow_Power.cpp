// MainWindow_Power.cpp — brick3-angelia: the title bar's POWER button, as
// Thetis has it. On connects to the last radio this computer connected to;
// off stops it, exactly as the radio tab's Disconnect does. The button is lit
// while connected and amber while a connect it started is under way.
//
// The radio is remembered under its own key ("PowerButtonRadio"): a
// disconnect by the operator clears LastConnectedRadioSerial (so the next
// start does not auto-connect), and POWER must still know what to switch on.

#include "MainWindow.h"

#include "ConnectionPanel.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "SpectrumWidget.h"
#include "TitleBar.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "models/RadioModel.h"

#include <QTimer>

namespace AetherSDR {

namespace {

const QString kPowerRadioKey = QStringLiteral("PowerButtonRadio");
const QString kPowerRadioNameKey = QStringLiteral("PowerButtonRadioName");
constexpr int kStartTimeoutMs = 20000;   // the amber state gives up after this

}  // namespace

void MainWindow::refreshPowerButton()
{
    if (!m_titleBar)
        return;
    auto& settings = AppSettings::instance();
    const QString name = settings.value(kPowerRadioNameKey).toString();
    const QString radio = name.isEmpty() ? tr("the last radio") : name;
    if (m_radioModel.isConnected()) {
        m_titleBar->setPowerState(TitleBar::PowerState::On,
            tr("Power is on: receiving from %1. Click to stop the radio and disconnect.")
                .arg(radio));
    } else if (m_powerStarting) {
        m_titleBar->setPowerState(TitleBar::PowerState::Starting,
            tr("Starting %1…").arg(radio));
    } else if (settings.value(kPowerRadioKey).toString().isEmpty()) {
        m_titleBar->setPowerState(TitleBar::PowerState::Off,
            tr("Power is off. Click to choose a radio and connect."));
    } else {
        m_titleBar->setPowerState(TitleBar::PowerState::Off,
            tr("Power is off. Click to connect to %1.").arg(radio));
    }
}

void MainWindow::refreshPauseButton()
{
    if (!m_titleBar)
        return;
    const bool available = m_radioModel.isConnected();
    m_titleBar->setPauseState(m_displayPaused, available,
        !available ? tr("Pause: freezes the spectrum and waterfall. Connect to a radio first.")
        : m_displayPaused
            ? tr("Paused: the spectrum and waterfall are frozen and this computer's "
                 "audio is muted; the radio keeps running. Click to resume: the "
                 "waterfall carries on right under the frozen rows.")
            : tr("Pause: freeze the spectrum and waterfall and mute this computer's "
                 "audio, e.g. while you change antennas. Resume carries on right "
                 "under the frozen rows, so the two can be compared at a glance."));
}

void MainWindow::setDisplayPaused(bool paused)
{
    if (paused && !m_radioModel.isConnected())
        paused = false;
    if (paused != m_displayPaused) {
        m_displayPaused = paused;
        if (m_panStack) {
            for (auto* applet : m_panStack->allApplets()) {
                if (SpectrumWidget* sw = applet ? applet->spectrumWidget() : nullptr)
                    sw->setDisplayFrozen(paused);
            }
        }
        if (m_audio) {
            if (paused) {
                // Only a mute the pause made is undone on resume.
                m_pauseMutedAudio = !m_audio->isMuted();
                if (m_pauseMutedAudio)
                    m_audio->setMuted(true);
            } else if (m_pauseMutedAudio) {
                m_pauseMutedAudio = false;
                if (m_audio->isMuted())
                    m_audio->setMuted(false);
            }
        }
    }
    refreshPauseButton();
}

void MainWindow::wirePowerButton()
{
    if (!m_titleBar || !m_connPanel)
        return;

    m_powerStartTimer = new QTimer(this);
    m_powerStartTimer->setSingleShot(true);
    m_powerStartTimer->setInterval(kStartTimeoutMs);
    connect(m_powerStartTimer, &QTimer::timeout, this, [this] {
        m_powerStarting = false;
        refreshPowerButton();
    });

    connect(m_titleBar, &TitleBar::powerRequested, this, [this](bool on) {
        if (!on) {
            m_powerStarting = false;
            m_powerStartTimer->stop();
            if (m_radioModel.isConnected())
                emit m_connPanel->disconnectRequested();
            refreshPowerButton();
            return;
        }
        if (m_radioModel.isConnected()) {
            refreshPowerButton();
            return;
        }
        // The picker's own Connect, on the remembered radio: the same slot
        // check, settings and connecting overlay as a click there.
        const QString serial = AppSettings::instance().value(kPowerRadioKey).toString();
        QString error;
        if (!serial.isEmpty() && m_connPanel->automationConnectLocalSerial(serial, &error)) {
            m_powerStarting = true;
            m_powerStartTimer->start();
            refreshPowerButton();
            return;
        }
        // No radio yet, or it has not answered discovery (switched off, still
        // booting): the picker, on that radio when it is listed.
        showConnectionDialog();
        if (!serial.isEmpty() && !m_connPanel->selectRadio(serial)) {
            m_connPanel->setStatusText(
                tr("The radio has not answered yet: check that it is switched on, "
                   "then connect to it here."));
        }
        refreshPowerButton();
    });

    connect(&m_radioModel, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (connected) {
            const RadioInfo info = m_radioModel.lastRadioInfo();
            if (!info.serial.isEmpty()) {
                auto& settings = AppSettings::instance();
                const QString name = m_connPanel->radioDisplayName(info, info.model);
                if (settings.value(kPowerRadioKey).toString() != info.serial
                    || settings.value(kPowerRadioNameKey).toString() != name) {
                    settings.setValue(kPowerRadioKey, info.serial);
                    settings.setValue(kPowerRadioNameKey, name);
                    settings.save();
                }
            }
        }
        m_powerStarting = false;
        m_powerStartTimer->stop();
        refreshPowerButton();
        // A pause ends with the connection; a new one starts unpaused.
        if (!connected)
            setDisplayPaused(false);
        refreshPauseButton();
    });
    connect(&m_radioModel, &RadioModel::connectionError, this, [this](const QString&) {
        m_powerStarting = false;
        m_powerStartTimer->stop();
        refreshPowerButton();
    });
    refreshPowerButton();

    connect(m_titleBar, &TitleBar::pauseRequested, this, [this](bool pause) {
        setDisplayPaused(pause);
    });
    refreshPauseButton();
}

}  // namespace AetherSDR
