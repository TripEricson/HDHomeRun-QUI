/*
 * mainwindow.cpp
 *
 * Main window: device discovery, tuning, signal status, PLP selection,
 * viewing in VLC, and ATSC 1.0 / 3.0 captures.
 *
 * HDHomeRun QUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QHostAddress>
#include <QDebug>
#include <QApplication>
#include <QThread>
#include <QStyle>
#include <QIcon>
#include <QMessageBox>
#include <QClipboard>
#include <QNetworkRequest>
#include <QUrl>
#include <QShortcut>
#include <QMenu>
#include <cerrno>
#include <climits>
#include <cstring>
#include <QIntValidator>
#include <QMap>
#include <QSet>
#include <QFileInfo>
#include <QTextStream>
#include <functional>
#include <memory>

// Include the parser here to avoid header conflicts in mainwindow.h
// l1_detail_parser.* and l1_details.* are the shared TUI/RENDER files, used
// unchanged. tuner.h / tuner_qui.c supply the tuner calls l1_details.c needs.
extern "C" {
#include "l1_details.h"
}
#include "tuner.h"

// Define specific constant if not in header
#ifndef MAX_DISPLAY_LINES
#define MAX_DISPLAY_LINES 1024
#endif

// =========================================================
// SAVE WORKER (ATSC 1.0)
// =========================================================
SaveWorker::SaveWorker(QString dev_str, QString filename, int duration, bool restart_on_err, int tuner_idx, QObject *parent)
    : QObject(parent), device_str(dev_str), file_name(filename), duration_sec(duration), restart_enabled(restart_on_err), tuner_index(tuner_idx), stop_requested(false) {}

SaveWorker::~SaveWorker() {}

void SaveWorker::request_stop() {
    stop_requested = true;
}

void SaveWorker::process() {
    // FIX #1: open a private device handle for this thread. libhdhomerun
    // handles are not thread-safe, and the GUI thread keeps polling status
    // on its own handle while we capture.
    struct hdhomerun_device_t *device = hdhomerun_device_create_from_str(device_str.toUtf8().constData(), NULL);
    if (!device) {
        emit finished("Error: Device not connected.");
        return;
    }
    hdhomerun_device_set_tuner(device, tuner_index);

    auto parse = [](const char* s, const char* k) -> long {
        const char* f = strstr(s, k);
        return f ? strtol(f + strlen(k), NULL, 0) : 0;
    };

    // Wait (here, off the GUI thread) for the tuner to lock after the
    // restore/retune that precedes a restart. Up to 5 s, stoppable.
    for (int i = 0; i < 50 && !stop_requested; i++) {
        struct hdhomerun_tuner_status_t st;
        char *raw = NULL;
        if (hdhomerun_device_get_tuner_status(device, &raw, &st) > 0 &&
            strcmp(st.lock_str, "none") != 0 && st.symbol_error_quality >= 100) break;
        QThread::msleep(100);
    }
    if (stop_requested) {
        hdhomerun_device_destroy(device);
        emit finished("Stopped.");
        return;
    }

    // Capture initial debug stats for error checking
    long start_te = 0, start_ne = 0, start_se = 0;
    char debug_path[64];
    snprintf(debug_path, sizeof(debug_path), "/tuner%d/debug", tuner_index);
    char *debug_str = NULL;

    // Always read the counters: they're reported at the end, as in the TUI
    if (hdhomerun_device_get_var(device, debug_path, &debug_str, NULL) > 0) {
        start_te = parse(debug_str, "te=");
        start_ne = parse(debug_str, "ne=");
        start_se = parse(debug_str, "se=");
    }

    if (hdhomerun_device_stream_start(device) <= 0) {
        hdhomerun_device_destroy(device);
        emit finished("Error: Failed to start stream.");
        return;
    }

    FILE *f = fopen(QFile::encodeName(file_name).constData(), "wb");
    if (!f) {
        hdhomerun_device_stream_stop(device);
        hdhomerun_device_destroy(device);
        emit finished("Error: Failed to open file.");
        return;
    }

    qint64 start_time = QDateTime::currentMSecsSinceEpoch();
    qint64 last_progress = -1000;
    qint64 last_check = 0;
    bool found_error = false;
    bool write_failed = false;
    int write_errno = 0;
    unsigned long long total_bytes = 0;

    while (!stop_requested) {
        qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - start_time;
        if (elapsed >= (qint64)duration_sec * 1000) break;

        // FIX #4: throttle progress to once per second instead of every loop pass
        if (elapsed - last_progress >= 1000) {
            last_progress = elapsed;
            emit progress(duration_sec - (int)(elapsed / 1000));
        }

        size_t actual_size = 0;
        uint8_t *video_data = hdhomerun_device_stream_recv(device, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);

        if (video_data && actual_size > 0) {
            if (fwrite(video_data, 1, actual_size, f) != actual_size) {
                write_failed = true;   // disk full, removed drive, ...
                write_errno = errno;
                break;
            }
            total_bytes += actual_size;
        } else {
            // FIX #4: stream_recv doesn't block; sleep like hdhomerun_config does
            QThread::msleep(64);
        }

        // FIX #4: check the error counters once per second, tracked by time
        if (restart_enabled && (elapsed - last_check >= 1000)) {
            last_check = elapsed;
            if (hdhomerun_device_get_var(device, debug_path, &debug_str, NULL) > 0) {
                long cur_te = parse(debug_str, "te=");
                long cur_ne = parse(debug_str, "ne=");
                long cur_se = parse(debug_str, "se=");

                if (cur_te > start_te || cur_ne > start_ne || cur_se > start_se) {
                    found_error = true;
                    stop_requested = true;
                }
            }
        }
    }

    if (fclose(f) != 0 && !write_failed) {   // buffered data can fail on close too
        write_failed = true;
        write_errno = errno;
    }
    hdhomerun_device_stream_stop(device);

    // Errors during the capture, for the summary
    long d_te = 0, d_ne = 0, d_se = 0;
    if (hdhomerun_device_get_var(device, debug_path, &debug_str, NULL) > 0) {
        d_te = parse(debug_str, "te=") - start_te;
        d_ne = parse(debug_str, "ne=") - start_ne;
        d_se = parse(debug_str, "se=") - start_se;
    }
    hdhomerun_device_destroy(device);

    if (write_failed) {
        emit finished(QString("Save stopped: couldn't write to %1 (%2).")
                      .arg(file_name, QString::fromLocal8Bit(strerror(write_errno))));
    } else if (found_error) {
        emit error_detected(file_name);
    } else if (total_bytes == 0) {
        // Nothing arrived: don't leave an empty file behind
        QFile::remove(file_name);
        if (stop_requested) {
            emit finished("Stopped; no data was saved.");
        } else if (restart_enabled) {
            emit error_detected(file_name);   // treat like any other error: retry
        } else {
            emit finished("Save failed: no data received from the tuner.");
        }
    } else {
        QString msg = QString("Saved %1 MB to %2 - Errors: %3 transport, %4 network, %5 sequence")
                          .arg((double)total_bytes / (1024.0*1024.0), 0, 'f', 2).arg(file_name)
                          .arg(d_te).arg(d_ne).arg(d_se);
        emit finished(msg);
    }
}


// Text colours, as in the TUI: green = locked, red = not locked / encrypted.
// Mid-tones, so they read on both light and dark themes.
static const QColor kGoodColor(46, 160, 67);
static const QColor kBadColor(209, 52, 56);

// Reads /tunerN/<name> by name with get_var (libhdhomerun's shortcut getters
// were getting error replies on the main connection). Returns an empty string
// if the device refuses, and logs the device's own error text.
static QString read_tuner_var(struct hdhomerun_device_t *dev, int tuner, const char *name)
{
    if (!dev) return QString();
    char path[48];
    snprintf(path, sizeof(path), "/tuner%d/%s", tuner, name);
    char *val = NULL;
    char *err = NULL;
    int r = hdhomerun_device_get_var(dev, path, &val, &err);
    if (r > 0 && val) return QString::fromUtf8(val);
    qDebug() << "get" << path << "failed (" << r << "):"
             << (err ? err : (r < 0 ? "communication error" : "no value"));
    return QString();
}

// =========================================================
// STATUS POLLER (own thread, own device handle)
// =========================================================

// Same test the GUI uses for "tech" models (parse_db_value(raw, "ss=") != -999)
static bool status_has_dbm(const char *raw) {
    const char *k = strstr(raw, "ss=");
    return k && strchr(k, '(') != NULL;
}

StatusPoller::~StatusPoller() {
    if (device) hdhomerun_device_destroy(device);
}

void StatusPoller::set_device(QString dev_str, int tuner, int generation) {
    if (device) { hdhomerun_device_destroy(device); device = nullptr; }
    current_generation = generation;
    tuner_index = tuner;
    if (dev_str.isEmpty()) return;
    device = hdhomerun_device_create_from_str(dev_str.toUtf8().constData(), NULL);
    if (device) hdhomerun_device_set_tuner(device, tuner);
}

void StatusPoller::poll() {
    TunerSnapshot snap;
    snap.generation = current_generation;
    snap.timestamp_ms = QDateTime::currentMSecsSinceEpoch();

    if (!device) { emit snapshot_ready(snap); return; }

    // Every char* from libhdhomerun is copied into a QString immediately,
    // because the next call on this handle overwrites the buffer.
    char *s = NULL;
    if (hdhomerun_device_get_tuner_status(device, &s, &snap.status) <= 0) {
        emit snapshot_ready(snap);
        return;
    }
    snap.ok = true;
    snap.raw_status = QString::fromUtf8(s);
    bool tech = status_has_dbm(s);

    bool locked = strcmp(snap.status.lock_str, "none") != 0;
    bool atsc3 = strstr(snap.status.lock_str, "atsc3") != NULL ||
                 strncmp(snap.status.channel, "atsc3:", 6) == 0;

    if (atsc3) {
        s = NULL;
        if (hdhomerun_device_get_tuner_plpinfo(device, &s) > 0 && s) snap.plpinfo = QString::fromUtf8(s);
    }
    if (locked) {
        s = NULL;
        if (hdhomerun_device_get_tuner_streaminfo(device, &s) > 0 && s) snap.streaminfo = QString::fromUtf8(s);
    }

    QByteArray debug_path = QString("/tuner%1/debug").arg(tuner_index).toLatin1();
    s = NULL;
    if (hdhomerun_device_get_var(device, debug_path.constData(), &s, NULL) > 0 && s) snap.debug = QString::fromUtf8(s);

    s = NULL;
    if (hdhomerun_device_get_tuner_target(device, &s) > 0 && s) snap.target = QString::fromUtf8(s);

    if (locked && atsc3 && tech) {
        QByteArray l1_path = QString("/tuner%1/l1detail").arg(tuner_index).toLatin1();
        s = NULL;
        if (hdhomerun_device_get_var(device, l1_path.constData(), &s, NULL) > 0 && s) snap.l1detail = QString::fromUtf8(s);
    }

    emit snapshot_ready(snap);
}

// =========================================================
// MAIN WINDOW
// =========================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // Version comes from CMakeLists.txt (project VERSION), via main.cpp
    this->setWindowTitle(QString("HDHomeRun Config QUI v%1").arg(QApplication::applicationVersion()));
    this->setWindowIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));

    active_device = nullptr;
    last_plp_freq = "";
    nam = new QNetworkAccessManager(this);
    current_reply = nullptr;
    save_file = nullptr;
    is_saving = false;
    save_is_atsc3 = false;
    is_seeking = false;
    seek_cancel_requested = false;
    current_tuner_index = 0;
    save_worker = nullptr;
    worker_thread = nullptr;

    vlc_process = nullptr;   // created per View in on_ViewButton_clicked()

    // Status polling runs in its own thread with its own device handle.
    qRegisterMetaType<TunerSnapshot>("TunerSnapshot");
    poller_thread = new QThread(this);
    poller = new StatusPoller;          // no parent: it moves to poller_thread
    poller->moveToThread(poller_thread);
    connect(poller, &StatusPoller::snapshot_ready, this, &MainWindow::handle_status_snapshot);
    poller_thread->start();

    status_timer = new QTimer(this);
    connect(status_timer, &QTimer::timeout, this, &MainWindow::request_status_poll);

    atsc3_monitor_timer = new QTimer(this);
    connect(atsc3_monitor_timer, &QTimer::timeout, this, &MainWindow::check_atsc3_status);

    countdown_timer = new QTimer(this);
    countdown_timer->setInterval(250);   // smooth second rollover
    connect(countdown_timer, &QTimer::timeout, this, &MainWindow::update_atsc3_countdown);

    restart_timer = new QTimer(this);
    restart_timer->setSingleShot(true);

    connect(restart_timer, &QTimer::timeout, this, [this](){
        if (is_saving) {
            // FIX #2: restart on the same path the save started on
            if (save_is_atsc3) {
                start_atsc3_save();
            } else {
                start_atsc1_save();
            }
        }
    });

    QSettings settings("HDHomeRunQUI", "Config");
    save_directory = settings.value("save_directory", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
    if (save_directory.isEmpty()) {
        save_directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }

    // These widgets are shown/hidden as the signal changes (ATSC 1.0 vs 3.0).
    // Keep their space reserved so the layout doesn't jump around.
    for (QWidget *w : std::initializer_list<QWidget*>{ui->L1CopyButton,
                                                      ui->L1InfoButton, ui->PLPCopyButton,
                                                      ui->A3SaveTypeBox, ui->A3SaveTypeLabel}) {
        if (!w) continue;
        QSizePolicy sp = w->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        w->setSizePolicy(sp);
    }

    // Seek buttons: the desktop theme's media icons (Breeze has them), falling
    // back to Qt's built-in ones -- much easier to see than the unicode arrows.
    ui->SeekDownButton->setText(QString());
    ui->SeekDownButton->setIcon(QIcon::fromTheme("media-seek-backward",
                                                 style()->standardIcon(QStyle::SP_MediaSeekBackward)));
    ui->SeekDownButton->setToolTip("Seek down to the previous channel with a signal (Ctrl+Left)");
    ui->SeekUpButton->setText(QString());
    ui->SeekUpButton->setIcon(QIcon::fromTheme("media-seek-forward",
                                               style()->standardIcon(QStyle::SP_MediaSeekForward)));
    ui->SeekUpButton->setToolTip("Seek up to the next channel with a signal (Ctrl+Right)");
    for (QPushButton *b : {ui->SeekDownButton, ui->SeekUpButton}) b->setIconSize(QSize(20, 20));

    // 0 shows as blank ("nothing selected"); the maximum comes from the channel map
    ui->ChannelWheel->setMinimum(0);
    ui->ChannelWheel->setSpecialValueText(" ");

    ui->BroadcastMapBox->addItems({"us-bcast", "us-cable", "us-hrc", "us-irc"});
    rebuild_channel_list(ui->BroadcastMapBox->currentText());

    // Channel map / channel number, and seek down / seek up, all the same
    // width: the grid splits spare space evenly between those two columns
    // (columnstretch 0,1,1,0 in the .ui), and each gets the widest one's
    // minimum so the style's natural sizes can't make them uneven.
    {
        const QList<QWidget*> same_width = { ui->BroadcastMapBox, ui->ChannelWheel,
                                             ui->SeekDownButton, ui->SeekUpButton };
        int w = 0;
        for (QWidget *x : same_width) w = qMax(w, x->sizeHint().width());
        for (QWidget *x : same_width) x->setMinimumWidth(w);
    }

    // Duration: whole seconds only, 1 s to 24 h
    ui->DurationBox->setValidator(new QIntValidator(1, 86400, ui->DurationBox));
    ui->DurationBox->setToolTip("Capture length in seconds (1 to 86400)");

    if(ui->BarStrength) ui->BarStrength->setValue(0);
    if(ui->BarSigQuality) ui->BarSigQuality->setValue(0);
    if(ui->BarSymQuality) ui->BarSymQuality->setValue(0);

    // FIX #9: on_ProgramBox_activated and on_PLPList_itemChanged are already
    // connected automatically by setupUi() (connectSlotsByName), so no manual
    // connect() here -- doing both made each slot fire twice.

    if (ui->L1CopyButton) ui->L1CopyButton->setVisible(false);
    if (ui->L1InfoButton) ui->L1InfoButton->setVisible(false);
    if (ui->PLPCopyButton) ui->PLPCopyButton->setVisible(false);

    if (ui->A3SaveTypeBox) ui->A3SaveTypeBox->setVisible(false);
    if (ui->A3SaveTypeLabel) ui->A3SaveTypeLabel->setVisible(false);

    // SLT BSID half of the ID row only appears on ATSC 3.0
    if (ui->LabelSLTBSID) ui->LabelSLTBSID->setVisible(false);
    if (ui->SLTBSIDBox) ui->SLTBSIDBox->setVisible(false);
    ui->TSIDBox->setText("Not Set");

    // FIX #5: QShortcut fires even when the matching button is disabled, so
    // every shortcut except Stop is ignored while a save or seek is running.
    auto add_shortcut = [this](const QKeySequence &seq, std::function<void()> fn) {
        QShortcut *sc = new QShortcut(seq, this);
        connect(sc, &QShortcut::activated, this, [this, fn]() {
            if (ui_busy()) return;
            fn();
        });
    };
    add_shortcut(QKeySequence(Qt::Key_S), [this](){ on_ScanButton_clicked(); });
    add_shortcut(QKeySequence(Qt::Key_V), [this](){ on_ViewButton_clicked(); });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), [this](){ on_SeekUpButton_clicked(); });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_Left),  [this](){ on_SeekDownButton_clicked(); });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_Up),    [this](){ ui->ChannelWheel->stepUp(); });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_Down),  [this](){ ui->ChannelWheel->stepDown(); });

    // Stop stays live while busy: it stops a save or cancels a seek.
    QShortcut *stopKey = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(stopKey, &QShortcut::activated, this, [this](){ on_StopButton_clicked(); });

    // Right-click a remembered address in the device list to forget it
    ui->DeviceList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->DeviceList, &QWidget::customContextMenuRequested, this, &MainWindow::show_device_menu);

    // First scan once the event loop is running, so the window appears first
    QTimer::singleShot(0, this, &MainWindow::on_ScanButton_clicked);
}

MainWindow::~MainWindow()
{
    // A scan still running must finish before we go (the window is already
    // hidden by now, so this wait isn't visible).
    if (scan_thread) {
        scan_thread->disconnect(this);
        scan_thread->wait();
        delete scan_thread;
        scan_thread = nullptr;
        if (is_scanning) QApplication::restoreOverrideCursor();
    }

    status_timer->stop();
    stop_current_save_operation();
    stop_view();   // also clears the tuner target so the device stops streaming

    poller_thread->quit();
    poller_thread->wait();
    delete poller;
    poller = nullptr;

    destroy_persistent_connection();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Let a running seek loop unwind promptly instead of finishing the band.
    seek_cancel_requested = true;
    QMainWindow::closeEvent(event);
}

// ... [Helper functions] ...

// FIX #10: connect by IP whenever we know it. Connecting by device ID needs
// broadcast discovery, which fails for devices only reachable by IP (routed
// subnet, second NIC, etc.). Returns just the IP (or ID as a fallback);
// callers set the tuner with hdhomerun_device_set_tuner() afterwards.
QString MainWindow::device_connect_string() const {
    return !current_device_ip.isEmpty() ? current_device_ip : current_device_id;
}

void MainWindow::create_persistent_connection() {
    if (active_device) return;
    QString dev_str = device_connect_string();
    if (dev_str.isEmpty()) return;
    active_device = hdhomerun_device_create_from_str(dev_str.toUtf8().constData(), NULL);
    if (active_device) {
        hdhomerun_device_get_name(active_device);
        hdhomerun_device_set_tuner(active_device, current_tuner_index);
        qDebug() << "Connected to" << QString("%1-%2").arg(current_device_id).arg(current_tuner_index)
                 << "at" << dev_str;
    }
}
void MainWindow::destroy_persistent_connection() {
    if (active_device) {
        hdhomerun_device_destroy(active_device);
        active_device = nullptr;
    }
}
struct hdhomerun_device_t* MainWindow::get_device_handle() { return active_device; }
long MainWindow::parse_status_value(const char *status_str, const char *key) {
    if (!status_str || !key) return -999;
    const char *found = strstr(status_str, key);
    if (found) return strtol(found + strlen(key), NULL, 0);
    return -999;
}
long MainWindow::parse_db_value(const char *status_str, const char *key) {
    if (!status_str || !key) return -999;
    const char *key_found = strstr(status_str, key);
    if (key_found) {
        const char *paren_open = strchr(key_found, '(');
        if (paren_open) return strtol(paren_open + 1, NULL, 10);
    }
    return -999;
}
long MainWindow::get_stream_id_value(struct hdhomerun_device_t* device, bool is_atsc3) {
    QByteArray info = read_tuner_var(device, current_tuner_index, is_atsc3 ? "plpinfo" : "streaminfo").toUtf8();
    if (info.isEmpty()) return -999;
    return parse_status_value(info.constData(), is_atsc3 ? "bsid=" : "tsid=");
}

// Channel number for a frequency in the current channel map: the entry whose
// centre frequency is closest, less than 3 MHz away (inside a 6 MHz channel;
// exactly 3 MHz is a band edge, not a channel). -1 if none.
int MainWindow::frequency_to_channel(long freq) {
    int best = -1;
    long best_diff = 3000000;
    for (int i = 0; i < channel_freqs.size(); i++) {
        long diff = labs(channel_freqs[i] - freq);
        if (diff < best_diff) { best_diff = diff; best = channel_numbers[i]; }
    }
    return best;
}

// Builds the channel list for a map (us-bcast, us-cable, us-hrc, us-irc, ...)
// from libhdhomerun's tables, so gaps (e.g. broadcast 37) and cable channels
// above 69 are handled. libhdhomerun orders entries by frequency; we sort by
// channel number, since that's how the spinner and seek step.
void MainWindow::rebuild_channel_list(const QString &channelmap) {
    QMap<int, long> by_number;
    struct hdhomerun_channel_list_t *list = hdhomerun_channel_list_create(channelmap.toUtf8().constData());
    if (list) {
        for (struct hdhomerun_channel_entry_t *e = hdhomerun_channel_list_first(list); e;
             e = hdhomerun_channel_list_next(list, e)) {
            by_number.insert(hdhomerun_channel_entry_channel_number(e), (long)hdhomerun_channel_entry_frequency(e));
        }
        hdhomerun_channel_list_destroy(list);
    }

    // US broadcast TV ends at 36 since the repack; libhdhomerun's us-bcast
    // table still lists 38-51 from before it.
    const int last_channel = (channelmap == "us-bcast") ? 36 : INT_MAX;

    channel_numbers.clear();
    channel_freqs.clear();
    for (auto it = by_number.cbegin(); it != by_number.cend(); ++it) {
        if (it.key() > last_channel) break;   // QMap iterates in channel order
        channel_numbers.append(it.key());
        channel_freqs.append(it.value());
    }
    if (channel_numbers.isEmpty()) {
        qDebug() << "No channel list for channel map" << channelmap;
    }

    // Read the value first: setting a lower maximum clamps it (e.g. cable 120
    // would become broadcast 51), and the clamp must not retune the tuner.
    int v = ui->ChannelWheel->value();
    ui->ChannelWheel->blockSignals(true);
    ui->ChannelWheel->setMaximum(channel_numbers.isEmpty() ? 0 : channel_numbers.last());
    ui->ChannelWheel->blockSignals(false);

    // Keep the channel if it exists in the new map, otherwise show blank
    set_wheel_silently(channel_numbers.contains(v) ? v : 0);
}

// Next channel in the map above (direction > 0) or below (< 0) 'from'.
// With wrap, runs off one end onto the other. -1 if there isn't one.
int MainWindow::next_valid_channel(int from, int direction, bool wrap) const {
    if (channel_numbers.isEmpty()) return -1;
    if (direction > 0) {
        for (int n : channel_numbers) if (n > from) return n;
        return wrap ? channel_numbers.first() : -1;
    }
    for (int i = channel_numbers.size() - 1; i >= 0; i--) {
        if (channel_numbers[i] < from) return channel_numbers[i];
    }
    return wrap ? channel_numbers.last() : -1;
}

// Moves the spinner without retuning.
void MainWindow::set_wheel_silently(int value) {
    ui->ChannelWheel->blockSignals(true);
    ui->ChannelWheel->setValue(value);
    ui->ChannelWheel->blockSignals(false);
    wheel_last_value = value;
}

void MainWindow::colorize_bar(QProgressBar* bar, int value, long db_val, QString unit) {
    QString color;
    if (value >= 75) color = "#4CAF50";
    else if (value >= 50) color = "#FFEB3B";
    else color = "#F44336";
    QString text_format;
    if (db_val != -999) text_format = QString("%p% (%1 %2)").arg(db_val).arg(unit);
    else text_format = "%p%";
    bar->setFormat(text_format);
    bar->setStyleSheet(QString("QProgressBar { text-align: center; font-weight: bold; color: black; border: 1px solid gray; border-radius: 3px; } QProgressBar::chunk { background-color: %1; width: 1px; }").arg(color));
}

// =========================================================
// SAVE FEATURE IMPLEMENTATION
// =========================================================

void MainWindow::on_SaveButton_clicked()
{
    if (is_saving) {
        stop_current_save_operation();
        return;
    }

    if (!active_device) return;

    bool duration_ok = false;
    int duration = ui->DurationBox->text().toInt(&duration_ok);
    if (!duration_ok || duration < 1 || duration > 86400) {
        QMessageBox::warning(this, "Save", "Enter a duration in seconds, from 1 to 86400.");
        ui->DurationBox->setFocus();
        ui->DurationBox->selectAll();
        return;
    }

    // A running VLC view and a save can't share the tuner target.
    stop_view();

    set_ui_enabled(false);
    is_saving = true;
    save_attempts = 0;
    plp_lock_checks = 0;
    ui->SaveButton->setText("Stop");

    struct hdhomerun_tuner_status_t status;
    char *raw_s;
    if (hdhomerun_device_get_tuner_status(active_device, &raw_s, &status) <= 0) {
        stop_current_save_operation();
        ui->statusbar->showMessage("Error reading status.");
        return;
    }

    // TUI LOGIC: Save the original channel string exactly as it is now.
    // This allows us to "restore" the tuner to this exact state (PLPs included)
    // if we need to restart or when we finish.
    saved_original_channel = QString::fromUtf8(status.channel);

    save_is_atsc3 = QString(status.lock_str).contains("atsc3");
    if (save_is_atsc3) {
        start_atsc3_save();
    } else {
        start_atsc1_save();
    }
}

void MainWindow::on_SetFolderButton_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Save Directory"),
                                                    save_directory,
                                                    QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        save_directory = dir;
        QSettings settings("HDHomeRunQUI", "Config");
        settings.setValue("save_directory", save_directory);
        ui->statusbar->showMessage("Save location set to: " + save_directory);
    }
}

void MainWindow::teardown_atsc3_transfer() {
    atsc3_monitor_timer->stop();
    countdown_timer->stop();

    if (current_reply) {
        current_reply->disconnect();
        current_reply->abort();
        current_reply->deleteLater();
        current_reply = nullptr;
    }
    if (save_file) {
        save_file->close();
        delete save_file;
        save_file = nullptr;
    }
}

// Mimics TUI `restore_and_exit` logic: re-tune to the original channel string.
// Non-blocking: we no longer sit in wait_for_lock on the GUI thread. The status
// poller shows the lock as it comes in, an ATSC 1.0 restart waits for lock
// inside the worker thread, and an ATSC 3.0 HTTP request tunes the tuner itself.
void MainWindow::restore_tuner_state() {
    if (!active_device) return;

    // Default fallback if we somehow didn't save state
    if (saved_original_channel.isEmpty()) {
        QString target_path = QString("/tuner%1/target").arg(current_tuner_index);
        hdhomerun_device_set_var(active_device, target_path.toStdString().c_str(), "none", NULL, NULL);
        return;
    }

    hdhomerun_device_set_tuner_channel(active_device, saved_original_channel.toStdString().c_str());
}

// An HTTP stream from port 5004 holds the tuner's lockkey. After we abort the
// request the device can keep holding it, so the next request for that same
// tuner gets "503 Service Unavailable". "lockkey force" frees it immediately.
//
// Only force it free if the lock is ours: /tunerN/lockkey reports "none" or
// the address of the client holding the lock. If a DVR or another app has
// taken the tuner, leave it alone and return that client's address.
// If the reply isn't an address we recognise, fall back to forcing it free,
// since that's what fixed the 503s.
QString MainWindow::release_tuner_from_http() {
    if (!active_device) return QString();

    QString holder = read_tuner_var(active_device, current_tuner_index, "lockkey").trimmed();
    QHostAddress holder_addr(holder);
    QString me = get_local_ip_address();

    if (holder.isEmpty() || holder.compare("none", Qt::CaseInsensitive) == 0) {
        // Not locked: nothing to free
    } else if (!holder_addr.isNull() && !me.isEmpty() && !holder_addr.isEqual(QHostAddress(me))) {
        qDebug() << "Tuner" << current_tuner_index << "is locked by" << holder
                 << "(this PC is" << me << ") - not forcing it free.";
        return holder;
    } else {
        if (hdhomerun_device_tuner_lockkey_force(active_device) <= 0) {
            qDebug() << "lockkey force failed on tuner" << current_tuner_index;
        }
    }

    QString target_path = QString("/tuner%1/target").arg(current_tuner_index);
    hdhomerun_device_set_var(active_device, target_path.toStdString().c_str(), "none", NULL, NULL);
    return QString();
}

void MainWindow::stop_current_save_operation() {
    bool was_saving = is_saving;
    teardown_atsc3_transfer();

    if (restart_timer->isActive()) restart_timer->stop();

    cleanup_atsc1_worker();

    QString taken_by;
    if (was_saving && save_is_atsc3) taken_by = release_tuner_from_http();

    // Use TUI restoration logic -- unless someone else has the tuner now,
    // in which case retuning it would fail (and isn't ours to do).
    if (taken_by.isEmpty()) restore_tuner_state();

    is_saving = false;
    set_ui_enabled(true);
    ui->SaveButton->setText("Save");
    if (taken_by.isEmpty()) {
        ui->statusbar->showMessage("Save stopped/finished.");
    } else {
        ui->statusbar->showMessage(QString("Save stopped. Tuner %1 is now in use by %2.")
                                   .arg(current_tuner_index).arg(taken_by));
    }
}

// FIX #3: single owner for the worker and its thread. Nothing uses
// deleteLater any more; the GUI thread stops, joins and deletes both, so
// there is never a dangling save_worker pointer.
void MainWindow::cleanup_atsc1_worker() {
    if (!worker_thread) return;
    if (save_worker) save_worker->request_stop();
    worker_thread->quit();
    worker_thread->wait();
    delete save_worker;
    save_worker = nullptr;
    delete worker_thread;
    worker_thread = nullptr;
}

void MainWindow::set_ui_enabled(bool enabled) {
    ui->ScanButton->setEnabled(enabled);
    ui->DeviceList->setEnabled(enabled);
    ui->ChannelWheel->setEnabled(enabled);
    ui->ViewButton->setEnabled(enabled);
    ui->SeekUpButton->setEnabled(enabled);
    ui->SeekDownButton->setEnabled(enabled);
    ui->ProgramBox->setEnabled(enabled);
    ui->BroadcastMapBox->setEnabled(enabled);
    ui->IPButton->setEnabled(enabled);
    if(ui->SetFolderButton) ui->SetFolderButton->setEnabled(enabled);
    // Changing PLPs or save options mid-capture would retune under the save
    if(ui->PLPList) ui->PLPList->setEnabled(enabled);
    if(ui->DurationBox) ui->DurationBox->setEnabled(enabled);
    if(ui->RestartCheckbox) ui->RestartCheckbox->setEnabled(enabled);
    if(ui->A3SaveTypeBox) ui->A3SaveTypeBox->setEnabled(enabled);
}

// --- ATSC 3.0 Logic ---

// Retries until it succeeds or you press Stop (no attempt limit, as in the TUI).
void MainWindow::perform_auto_restart() {
    {
        teardown_atsc3_transfer();
        remove_capture_files();

        // Free the tuner from the aborted HTTP stream (avoids 503 on retry),
        // then restore state to clear hardware buffers (TUI logic). If another
        // client has taken the tuner, a retry can't succeed: give up instead.
        QString taken_by = release_tuner_from_http();
        if (!taken_by.isEmpty()) {
            QString msg = QString("Capture stopped: tuner %1 is now in use by %2.")
                              .arg(current_tuner_index).arg(taken_by);
            stop_current_save_operation();
            ui->statusbar->showMessage(msg);
            QMessageBox::warning(this, "Save", msg);
            return;
        }
        restore_tuner_state();

        // Back off a little more on each attempt -- 2, 4, 6, 8 s -- then
        // hold at 10 s, so a long run of retries doesn't hammer the device.
        int delay_ms = qMin(2000 * save_attempts, 10000);
        ui->statusbar->showMessage(QString("Restarting capture in %1 s (attempt %2)...")
                                   .arg(delay_ms / 1000).arg(save_attempts + 1));

        set_ui_enabled(false);
        ui->SaveButton->setText("Stop");

        restart_timer->start(delay_ms);
    }
}

void MainWindow::start_atsc3_save() {
    save_attempts++;

    // Only configure basic params on first attempt, otherwise preserve cached state
    if (save_attempts == 1) {
        cached_duration = ui->DurationBox->text().toInt();
        if (cached_duration <= 0) cached_duration = 30;

        cached_format = "dbg";
        cached_is_pcap = false;

        if (ui->A3SaveTypeBox->isVisible()) {
            QString txt = ui->A3SaveTypeBox->currentText();
            if (txt == "ALP PCAP") { cached_format = "alp-pcap"; cached_is_pcap = true; }
            else if (txt == "IPv4 PCAP") { cached_format = "ipv4-pcap"; cached_is_pcap = true; }
        }

        cached_plp_str = "";
        // Try to get PLPs from UI list
        if (ui->PLPList->count() > 0) {
            QStringList plps;
            for(int i=0; i<ui->PLPList->count(); ++i) {
                QListWidgetItem* it = ui->PLPList->item(i);
                if (it->checkState() == Qt::Checked) {
                    plps.append(QString("p%1").arg(it->data(Qt::UserRole).toInt()));
                }
            }
            cached_plp_str = plps.join("");
        }

        // Fallback: Parse currently locked PLPs if none selected in UI
        if (cached_plp_str.isEmpty()) {
            QString pi = read_tuner_var(active_device, current_tuner_index, "plpinfo");
            if (!pi.isEmpty()) {
                QStringList lines = pi.split('\n');
                for(const QString &line : lines) {
                    if(line.contains("lock=1")) {
                        int id = line.split(':').first().toInt();
                        cached_plp_str += QString("p%1").arg(id);
                    }
                }
            }
        }
    }

    // Wait for every requested PLP to lock before starting, as the TUI does:
    // up to 3 more checks, 1 s apart (on a timer, so the window stays usable).
    {
        QSet<int> wanted;
        for (const QString &id : cached_plp_str.split('p', Qt::SkipEmptyParts)) wanted.insert(id.toInt());

        QSet<int> locked;
        QString pi = read_tuner_var(active_device, current_tuner_index, "plpinfo");
        for (const QString &line : pi.split('\n')) {
            int colon = line.indexOf(':');
            bool ok = false;
            int id = (colon > 0) ? line.left(colon).trimmed().toInt(&ok) : -1;
            if (ok && line.contains("lock=1")) locked.insert(id);
        }
        int locked_wanted = (wanted & locked).size();

        if (wanted.isEmpty() || locked_wanted < wanted.size()) {
            if (plp_lock_checks < 3) {
                plp_lock_checks++;
                save_attempts--;   // not an attempt yet
                ui->statusbar->showMessage(wanted.isEmpty()
                    ? QString("No locked PLPs yet, retrying... (%1/3)").arg(plp_lock_checks)
                    : QString("Waiting for all PLPs to lock (%1/%2)... (%3/3)")
                          .arg(locked_wanted).arg(wanted.size()).arg(plp_lock_checks));
                restart_timer->start(1000);   // comes back here; Stop cancels it
                return;
            }
            QString msg = wanted.isEmpty() ? "No locked PLPs found for the ATSC 3.0 capture."
                                           : "Could not lock all requested PLPs. Capture not started.";
            stop_current_save_operation();
            ui->statusbar->showMessage(msg);
            return;
        }
        plp_lock_checks = 0;
    }

    // Parse frequency from saved original channel string for filename/url
    QString freq_part = "0";
    if (!saved_original_channel.isEmpty()) {
        QStringList parts = saved_original_channel.split(':');
        if (parts.size() > 1) freq_part = parts[1];
    }

    // Try to convert freq to channel number for cleaner filename
    bool ok;
    long freq_val = freq_part.toLong(&ok);
    if (ok && freq_val > 1000000) {
        int ch = frequency_to_channel(freq_val);
        if (ch > 0) freq_part = QString::number(ch);
    }

    // Same name as the TUI: rf<ch>-bsid<id>-<plps>-<time>.<ext>. The ID is the
    // BSID, else the TSID, else 0 -- also as the TUI does it.
    long id = get_stream_id_value(active_device, true);
    if (id == -999) id = get_stream_id_value(active_device, false);
    if (id == -999) id = 0;

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString ext = cached_is_pcap ? ".pcap" : ".dbg";
    QString file_only = QString("rf%1-bsid%2-%3-%4%5").arg(freq_part).arg(id).arg(cached_plp_str).arg(timestamp).arg(ext);

    current_save_filename = QDir(save_directory).filePath(file_only);

    // URL construction requires the raw frequency part from the original channel string usually
    QString raw_freq = "0";
    if (!saved_original_channel.isEmpty()) {
        QStringList parts = saved_original_channel.split(':');
        if (parts.size() > 1) raw_freq = parts[1];
    }

    QString url_str = QString("http://%1:5004/tuner%2/ch%3%4?format=%5&duration=%6")
                          .arg(current_device_ip)
                          .arg(current_tuner_index)
                          .arg(raw_freq)
                          .arg(cached_plp_str)
                          .arg(cached_format)
                          .arg(cached_duration);

    qDebug() << "Saving URL:" << url_str;

    teardown_atsc3_transfer();

    save_file = new QFile(current_save_filename);
    if (!save_file->open(QIODevice::WriteOnly)) {
        stop_current_save_operation();
        QMessageBox::critical(this, "Error", "Could not open file for writing.");
        return;
    }

    // The .txt goes first: the HTTP request below makes the device retune,
    // and for a moment PLP info and the L1 string aren't available.
    write_l1_sidecar();

    QNetworkRequest request(url_str);
    // FIX: This header is MANDATORY for the device to release the lock immediately upon finish/abort
    request.setRawHeader("Connection", "close");

    current_reply = nam->get(request);

    connect(current_reply, &QNetworkReply::readyRead, this, &MainWindow::handle_atsc3_readyRead);
    connect(current_reply, &QNetworkReply::finished, this, &MainWindow::handle_atsc3_save_finished);

    ui->SaveButton->setText("Stop");
    ui->statusbar->showMessage(QString("Saving ATSC 3.0 to %1...").arg(current_save_filename));

    // The HTTP request makes the device retune, so the signal looks bad for
    // the first second or two. check_atsc3_status() waits for a clean lock
    // before it starts treating drops as errors.
    atsc3_save_started_ms = QDateTime::currentMSecsSinceEpoch();
    atsc3_seen_good = false;
    if (ui->RestartCheckbox->isChecked()) {
        atsc3_monitor_timer->start(1000);
    }

    countdown_end_ms = atsc3_save_started_ms + (qint64)cached_duration * 1000;
    update_atsc3_countdown();
    countdown_timer->start();
}

void MainWindow::handle_atsc3_readyRead() {
    if (!current_reply || !save_file) return;
    // Don't write an HTTP error page (e.g. a 503 body) into the capture file.
    int http_status = current_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http_status != 0 && http_status != 200) return;

    QByteArray data = current_reply->readAll();
    if (data.isEmpty()) return;
    if (save_file->write(data) != data.size()) {
        // Disk full, removed drive, etc. Keep what was written and stop.
        QString why = save_file->errorString();
        QString name = current_save_filename;
        stop_current_save_operation();
        ui->statusbar->showMessage(QString("Save stopped: couldn't write to %1 (%2).").arg(name, why));
        QMessageBox::critical(this, "Save", QString("Couldn't write to %1:\n%2").arg(name, why));
    }
}

void MainWindow::handle_atsc3_save_finished() {
    if (!is_saving || !current_reply) return;

    QNetworkReply::NetworkError err = current_reply->error();
    int http_status = current_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (err == QNetworkReply::NoError) {
        handle_atsc3_readyRead();   // drain anything left in the buffer
        if (!is_saving || !save_file) return;   // a write error already stopped the save

        qint64 bytes = save_file->size();
        if (bytes == 0) {
            // The device closed the stream without sending anything
            qDebug() << "ATSC 3.0 save: no data received.";
            if (ui->RestartCheckbox->isChecked()) {
                perform_auto_restart();
                return;
            }
            teardown_atsc3_transfer();
            remove_capture_files();
            stop_current_save_operation();
            ui->statusbar->showMessage("Save failed: no data received from the tuner.");
            return;
        }

        QString done = QString("Saved %1 MB to %2")
                           .arg((double)bytes / (1024.0*1024.0), 0, 'f', 2)
                           .arg(current_save_filename);
        stop_current_save_operation();
        ui->statusbar->showMessage(done);
        return;
    }

    // Log what the device actually said -- HDHomeRun puts the reason in the body.
    QString reason = current_reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    QString body = QString::fromUtf8(current_reply->readAll()).trimmed().left(200);
    qDebug() << "Network Error during save:" << err << "HTTP" << http_status << reason << body;

    if (ui->RestartCheckbox->isChecked()) {
        perform_auto_restart();
        return;
    }

    QString msg = QString("Download Error: %1").arg(current_reply->errorString());
    if (!body.isEmpty()) msg += " (" + body + ")";
    teardown_atsc3_transfer();   // close the file before deleting it
    remove_capture_files();
    stop_current_save_operation();
    ui->statusbar->showMessage(msg);   // after stop, so it isn't overwritten
}

// Runs once a second during an ATSC 3.0 save when "Restart on Error" is on.
// Uses the poller's latest snapshot, so it never blocks the GUI.
void MainWindow::check_atsc3_status() {
    if (!is_saving || !save_is_atsc3) return;

    const TunerSnapshot &snap = last_snapshot;
    // Only trust a snapshot taken after this attempt's request went out
    if (!snap.ok || snap.timestamp_ms < atsc3_save_started_ms) return;

    qint64 since_start = QDateTime::currentMSecsSinceEpoch() - atsc3_save_started_ms;
    bool locked = strcmp(snap.status.lock_str, "none") != 0;
    bool clean = locked && snap.status.symbol_error_quality >= 100;

    if (!atsc3_seen_good) {
        // Grace period: the HTTP request retunes the tuner, so wait for the
        // first clean reading before counting anything as an error.
        if (clean) {
            atsc3_seen_good = true;
            qDebug() << "ATSC 3.0 save: clean lock after" << since_start << "ms";
        } else if (since_start > 10000) {
            qDebug() << "ATSC 3.0 save: no clean lock within 10 s.";
            perform_auto_restart();
        }
        return;
    }

    if (!clean) {
        qDebug() << "Signal Quality Drop Detected during Save. seq =" << snap.status.symbol_error_quality
                 << "lock =" << snap.status.lock_str;
        perform_auto_restart();
    }
}

// --- ATSC 1.0 Logic ---

void MainWindow::start_atsc1_save() {
    save_attempts++;

    int duration = ui->DurationBox->text().toInt();
    if (duration <= 0) duration = 30;

    QString freq_part = "0";
    if (!saved_original_channel.isEmpty()) {
        // Handle standard "auto:33" or "ch33" formats
        if (saved_original_channel.contains(":")) {
            freq_part = saved_original_channel.split(':').value(1, "0");
        } else if (saved_original_channel.startsWith("ch")) {
            freq_part = saved_original_channel.mid(2);
        }
    }

    bool ok;
    long freq_val = freq_part.toLong(&ok);
    if (ok && freq_val > 1000000) {
        int ch = frequency_to_channel(freq_val);
        if (ch > 0) freq_part = QString::number(ch);
    }

    long id = get_stream_id_value(active_device, false);
    if (id == -999) id = 0;   // as the TUI names it when there's no TSID

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString file_only = QString("rf%1-tsid%2-%3.ts").arg(freq_part).arg(id).arg(timestamp);

    current_save_filename = QDir(save_directory).filePath(file_only);

    cleanup_atsc1_worker();   // never leave a previous worker behind

    worker_thread = new QThread;
    save_worker = new SaveWorker(device_connect_string(), current_save_filename, duration, ui->RestartCheckbox->isChecked(), current_tuner_index);
    save_worker->moveToThread(worker_thread);

    connect(worker_thread, &QThread::started, save_worker, &SaveWorker::process);
    connect(save_worker, &SaveWorker::finished, this, &MainWindow::handle_atsc1_worker_finished);
    connect(save_worker, &SaveWorker::error_detected, this, &MainWindow::handle_atsc1_worker_error);
    connect(save_worker, &SaveWorker::progress, this, &MainWindow::handle_atsc1_worker_progress);
    // FIX #3: no deleteLater here -- cleanup_atsc1_worker() owns deletion.

    ui->SaveButton->setText("Stop");

    worker_thread->start();
    ui->statusbar->showMessage(QString("Saving ATSC 1.0 to %1...").arg(current_save_filename));
}

void MainWindow::handle_atsc1_worker_progress(int remaining) {
    if (!is_saving) return;
    show_save_remaining(remaining);
}

// Shared countdown display: button shows "Stop (0:27)", status bar names the file.
void MainWindow::show_save_remaining(int seconds)
{
    if (seconds < 0) seconds = 0;
    QString t = QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
    ui->SaveButton->setText(QString("Stop (%1)").arg(t));
    QString name = QFileInfo(current_save_filename).fileName();
    if (seconds > 0) {
        ui->statusbar->showMessage(QString("Saving ATSC %1 - %2 remaining - %3")
                                   .arg(save_is_atsc3 ? "3.0" : "1.0").arg(t).arg(name));
    } else {
        ui->statusbar->showMessage(QString("Saving ATSC %1 - finishing - %2")
                                   .arg(save_is_atsc3 ? "3.0" : "1.0").arg(name));
    }
}

void MainWindow::update_atsc3_countdown()
{
    if (!is_saving || !save_is_atsc3) { countdown_timer->stop(); return; }
    qint64 left_ms = countdown_end_ms - QDateTime::currentMSecsSinceEpoch();
    int left = left_ms > 0 ? (int)((left_ms + 999) / 1000) : 0;   // round up
    show_save_remaining(left);
}

// "rf33-p0p1-20261113-150400.pcap" -> "rf33-p0p1-20261113-150400.txt"
QString MainWindow::l1_sidecar_filename() const
{
    if (current_save_filename.isEmpty()) return QString();
    QFileInfo fi(current_save_filename);
    return fi.dir().filePath(fi.completeBaseName() + ".txt");
}

// Writes the same details .txt the TUI saves with its captures (PLP info with
// required SNR, IDs, the full L1 parse and the raw L1 string), using the TUI's
// save_atsc3_details_auto() from l1_details.c. Uses its own connection.
void MainWindow::write_l1_sidecar()
{
    struct hdhomerun_device_t *dev =
        hdhomerun_device_create_from_str(device_connect_string().toUtf8().constData(), NULL);
    if (!dev) {
        qDebug() << "Details .txt: couldn't open a connection to the tuner.";
        return;
    }
    hdhomerun_device_set_tuner(dev, current_tuner_index);
    struct tuner t = { dev, current_tuner_index };

    // It replaces the capture's extension with .txt -- same as l1_sidecar_filename()
    QByteArray fname = QFile::encodeName(current_save_filename);
    if (save_atsc3_details_auto(&t, fname.constData()) != 0) {
        qDebug() << "Details .txt: not written (no PLP info from the tuner?)";
    }
    hdhomerun_device_destroy(dev);
}

void MainWindow::remove_capture_files()
{
    if (current_save_filename.isEmpty()) return;
    QFile::remove(current_save_filename);
    QString txt = l1_sidecar_filename();
    if (!txt.isEmpty()) QFile::remove(txt);
}

void MainWindow::handle_atsc1_worker_finished(QString msg) {
    // FIX #3: a queued 'finished' can arrive after the user already pressed
    // Stop (which cleaned everything up). Ignore it then.
    if (!is_saving) return;
    stop_current_save_operation();
    ui->statusbar->showMessage(msg);   // after stop, so it isn't overwritten
}

void MainWindow::handle_atsc1_worker_error(QString filename) {
    if (!is_saving) return;
    QFile::remove(filename);

    // Retries until it succeeds or you press Stop (no attempt limit, as in the TUI)
    cleanup_atsc1_worker();

    // TUI LOGIC: Restore state to clear hardware buffers
    restore_tuner_state();

    ui->statusbar->showMessage(QString("Error detected. Restarting capture (attempt %1)...").arg(save_attempts + 1));

    set_ui_enabled(false);
    ui->SaveButton->setText("Stop");

    restart_timer->start(2000);
}

// =========================================================
// SCANNING & SELECTION
// =========================================================
// Drops the current tuner (VLC view, connections, status thread) and empties
// the device list, ready for a fresh list.
void MainWindow::forget_current_device(bool clear_list)
{
    on_StopButton_clicked();   // stops VLC and clears the tuner target
    status_timer->stop();
    destroy_persistent_connection();
    // Tell the status thread to drop its handle too
    device_generation++;
    {
        int gen = device_generation;
        StatusPoller *p = poller;
        QMetaObject::invokeMethod(poller, [p, gen]() { p->set_device(QString(), 0, gen); }, Qt::QueuedConnection);
    }
    if (clear_list) ui->DeviceList->clear();
    clear_tuned_channel_info();
}

// Runs a discovery job on a background thread with the controls greyed out,
// then calls done() back on the GUI thread with what it found.
void MainWindow::run_scan_in_background(std::function<QList<FoundDevice>()> job, const QString &busy_msg,
                                        std::function<void(const QList<FoundDevice> &)> done)
{
    is_scanning = true;
    set_ui_enabled(false);
    ui->SaveButton->setEnabled(false);
    ui->statusbar->showMessage(busy_msg);
    QApplication::setOverrideCursor(Qt::BusyCursor);

    auto result = std::make_shared<QList<FoundDevice>>();
    scan_thread = QThread::create([job, result]() { *result = job(); });
    connect(scan_thread, &QThread::finished, this, [this, result, done]() {
        scan_thread->deleteLater();
        scan_thread = nullptr;
        is_scanning = false;
        QApplication::restoreOverrideCursor();
        set_ui_enabled(true);
        ui->SaveButton->setEnabled(true);
        done(*result);
    });
    scan_thread->start();
}

void MainWindow::on_ScanButton_clicked()
{
    // Scanning destroys the device handle; never do that under a save/seek/scan.
    if (ui_busy()) return;
    forget_current_device();

    QStringList ips = remembered_ips();
    run_scan_in_background([ips]() { return discover_all_devices(ips); }, "Scanning...",
                           [this](const QList<FoundDevice> &found) {
        int tuners = 0, missing = 0;
        for (const FoundDevice &d : found) {
            if (d.tuner_count > 0) {
                add_device_tuners(d.ip, d.device_id, d.tuner_count, d.remembered, d.discovered);
                tuners += d.tuner_count;
            } else {
                add_unreachable_device(d.ip);
                missing++;
            }
        }

        QString msg = tuners > 0 ? QString("Found %1 tuners.").arg(tuners) : QString("No devices found.");
        if (missing > 0) msg += QString(" %1 remembered address%2 didn't answer.").arg(missing).arg(missing > 1 ? "es" : "");
        ui->statusbar->showMessage(msg);
        select_first_tuner();
    });
}

// Background part of Scan: broadcast discovery, then the hdhomerun_config
// fallback, then the remembered addresses. No UI access here -- it runs on
// scan_thread.
QList<MainWindow::FoundDevice> MainWindow::discover_all_devices(const QStringList &remembered_ips)
{
    QList<FoundDevice> found;

    struct hdhomerun_discover_t *ds = hdhomerun_discover_create(NULL);
    if (ds) {
        uint32_t device_types[1] = { HDHOMERUN_DEVICE_TYPE_TUNER };
        // IPv4 broadcast discovery (as in the TUI)
        if (hdhomerun_discover2_find_devices_broadcast(ds, HDHOMERUN_DISCOVER_FLAGS_IPV4_GENERAL, device_types, 1) > 0) {
            for (struct hdhomerun_discover2_device_t *device = hdhomerun_discover2_iter_device_first(ds);
                 device; device = hdhomerun_discover2_iter_device_next(device)) {
                FoundDevice d;
                d.device_id = QString::number(hdhomerun_discover2_device_get_device_id(device), 16).toUpper();
                d.tuner_count = hdhomerun_discover2_device_get_tuner_count(device);

                struct hdhomerun_discover2_device_if_t *device_if = hdhomerun_discover2_iter_device_if_first(device);
                char ip_str[64] = {0};
                if (device_if) {
                    struct sockaddr_storage ip_address;
                    hdhomerun_discover2_device_if_get_ip_addr(device_if, &ip_address);
                    hdhomerun_sock_sockaddr_to_ip_str(ip_str, (struct sockaddr *)&ip_address, true);
                }
                d.ip = QString::fromUtf8(ip_str);
                d.discovered = true;
                if (d.tuner_count > 0) found.append(d);
            }
        }
        hdhomerun_discover_destroy(ds);
    }

    // Fallback kept just in case, but discovery above should suffice
    if (found.isEmpty()) {
        qDebug() << "Library scan failed/empty. Trying 'hdhomerun_config discover'...";
        QProcess discoverProc;
        discoverProc.start("hdhomerun_config", QStringList() << "discover");
        discoverProc.waitForFinished(5000);   // off the GUI thread now, so it can take its time

        QString output = discoverProc.readAllStandardOutput();
        for (const QString &line : output.split('\n')) {
            if (!line.startsWith("hdhomerun device", Qt::CaseInsensitive)) continue;
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 6) continue;
            QString ip_addr = parts[5].trimmed();

            // Ask the device for its real tuner count
            FoundDevice d;
            d.ip = ip_addr;
            d.discovered = true;
            if (detect_device(ip_addr, &d.device_id, &d.tuner_count)) {
                found.append(d);
            } else {
                qDebug() << "hdhomerun_config listed" << ip_addr << "but it didn't answer.";
            }
        }
    }

    // Remembered addresses (from Enter IP). Ones discovery already found are
    // just marked; the rest are asked directly; no answer -> tuner_count 0.
    for (const QString &ip : remembered_ips) {
        bool already = false;
        for (FoundDevice &f : found) {
            if (f.ip == ip) { f.remembered = true; already = true; break; }
        }
        if (already) continue;

        FoundDevice d;
        d.ip = ip;
        d.remembered = true;
        if (detect_device(ip, &d.device_id, &d.tuner_count)) {
            // Same device, found by discovery under another address?
            for (FoundDevice &f : found) {
                if (f.device_id == d.device_id) { f.remembered = true; already = true; break; }
            }
            if (already) continue;
        } else {
            d.tuner_count = 0;
        }
        found.append(d);
    }
    return found;
}

// Asks the device itself for its ID and tuner count, instead of guessing from
// the device ID. First a targeted discovery (UDP to that one address, which
// reports the tuner count); if that gets no answer -- some routed networks drop
// it -- falls back to the TCP control connection and probes /tunerN/status.
bool MainWindow::detect_device(const QString &ip, QString *device_id_out, int *tuner_count_out)
{
    *device_id_out = QString();
    *tuner_count_out = 0;

    struct sockaddr_storage addr;
    if (hdhomerun_sock_ip_str_to_sockaddr(ip.toUtf8().constData(), &addr)) {
        struct hdhomerun_discover_t *ds = hdhomerun_discover_create(NULL);
        if (ds) {
            uint32_t types[1] = { HDHOMERUN_DEVICE_TYPE_TUNER };
            if (hdhomerun_discover2_find_devices_targeted(ds, (const struct sockaddr *)&addr, types, 1) > 0) {
                struct hdhomerun_discover2_device_t *d = hdhomerun_discover2_iter_device_first(ds);
                if (d) {
                    *device_id_out = QString::number(hdhomerun_discover2_device_get_device_id(d), 16).toUpper();
                    *tuner_count_out = hdhomerun_discover2_device_get_tuner_count(d);
                }
            }
            hdhomerun_discover_destroy(ds);
        }
        if (!device_id_out->isEmpty() && *tuner_count_out > 0) return true;
    }

    // Fallback: control connection
    struct hdhomerun_device_t *dev = hdhomerun_device_create_from_str(ip.toUtf8().constData(), NULL);
    if (!dev) return false;

    uint32_t id = hdhomerun_device_get_device_id(dev);
    if (id == 0) {                      // nothing answered at that address
        hdhomerun_device_destroy(dev);
        return false;
    }
    *device_id_out = QString::number(id, 16).toUpper();

    int count = 0;
    for (int t = 0; t < 16; t++) {
        char path[32];
        char *val = NULL;
        snprintf(path, sizeof(path), "/tuner%d/status", t);
        if (hdhomerun_device_get_var(dev, path, &val, NULL) <= 0) break;   // no such tuner
        count++;
    }
    hdhomerun_device_destroy(dev);

    *tuner_count_out = count;
    return count > 0;
}

void MainWindow::add_device_tuners(const QString &ip, const QString &device_id, int tuner_count,
                                   bool remembered, bool discovered)
{
    for (int t = 0; t < tuner_count; t++) {
        QString label = QString("%1-%2").arg(device_id).arg(t);
        QListWidgetItem* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, ip);
        item->setData(Qt::UserRole + 1, t);
        item->setData(Qt::UserRole + 2, device_id);
        item->setData(Qt::UserRole + 3, remembered);
        item->setData(Qt::UserRole + 4, discovered);
        item->setToolTip(remembered ? QString("%1 (remembered; right-click to forget)").arg(ip) : ip);
        ui->DeviceList->addItem(item);
    }
}

// A remembered address that didn't answer: shown greyed out so it can still
// be forgotten (right-click), but can't be selected.
void MainWindow::add_unreachable_device(const QString &ip)
{
    QListWidgetItem *item = new QListWidgetItem(ip + " (not found)");
    item->setData(Qt::UserRole, ip);
    item->setData(Qt::UserRole + 3, true);
    item->setData(Qt::UserRole + 4, false);
    item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
    item->setToolTip(QString("No answer from %1 at the last scan. Right-click to forget it.").arg(ip));
    ui->DeviceList->addItem(item);
}

void MainWindow::select_first_tuner()
{
    for (int i = 0; i < ui->DeviceList->count(); i++) {
        QListWidgetItem *it = ui->DeviceList->item(i);
        if (it->flags() & Qt::ItemIsEnabled) {
            ui->DeviceList->setCurrentRow(i);
            on_DeviceList_itemClicked(it);
            return;
        }
    }
}

QStringList MainWindow::remembered_ips() const
{
    QSettings settings("HDHomeRunQUI", "Config");
    return settings.value("RememberedIPs").toStringList();
}

void MainWindow::set_remembered_ips(const QStringList &ips)
{
    QSettings settings("HDHomeRunQUI", "Config");
    settings.setValue("RememberedIPs", ips);
}

void MainWindow::remember_ip(const QString &ip)
{
    QStringList ips = remembered_ips();
    if (!ips.contains(ip)) {
        ips.append(ip);
        set_remembered_ips(ips);
    }
}

// Forgets an address. Devices that only came from it leave the list (and if
// one was in use, it's disconnected); a device Scan also found just stops
// being marked as remembered.
void MainWindow::forget_remembered_ip(const QString &ip)
{
    QStringList ips = remembered_ips();
    ips.removeAll(ip);
    set_remembered_ips(ips);

    bool removed_current = false;
    for (int i = ui->DeviceList->count() - 1; i >= 0; i--) {
        QListWidgetItem *it = ui->DeviceList->item(i);
        if (it->data(Qt::UserRole).toString() != ip || !it->data(Qt::UserRole + 3).toBool()) continue;
        if (it->data(Qt::UserRole + 4).toBool()) {
            it->setData(Qt::UserRole + 3, false);
            it->setToolTip(ip);
            continue;
        }
        if (active_device && it->data(Qt::UserRole + 2).toString() == current_device_id) removed_current = true;
        delete ui->DeviceList->takeItem(i);
    }

    ui->statusbar->showMessage("Forgot " + ip + ".");
    if (removed_current) {
        forget_current_device(false);
        select_first_tuner();
    }
}

void MainWindow::show_device_menu(const QPoint &pos)
{
    if (ui_busy()) return;
    QListWidgetItem *item = ui->DeviceList->itemAt(pos);
    if (!item || !item->data(Qt::UserRole + 3).toBool()) return;   // only remembered addresses

    QString ip = item->data(Qt::UserRole).toString();
    QMenu menu(this);
    QAction *forget = menu.addAction(QString("Forget %1").arg(ip));
    if (menu.exec(ui->DeviceList->viewport()->mapToGlobal(pos)) == forget) {
        forget_remembered_ip(ip);
    }
}

void MainWindow::on_IPButton_clicked()
{
    if (ui_busy()) return;

    bool ok;
    QString ip = QInputDialog::getText(this, "Manual IP", "Enter HDHomeRun IP Address:", QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || ip.isEmpty()) return;

    // Detect in the background. The current device stays connected until we
    // know the new one exists, so a typo doesn't cost you your tuner.
    run_scan_in_background([ip]() {
        QList<FoundDevice> found;
        FoundDevice d;
        d.ip = ip;
        if (detect_device(ip, &d.device_id, &d.tuner_count)) found.append(d);
        return found;
    }, "Connecting to " + ip + "...", [this, ip](const QList<FoundDevice> &found) {
        if (found.isEmpty()) {
            ui->statusbar->showMessage("No HDHomeRun found at " + ip);
            QMessageBox::warning(this, "Error", "Could not find an HDHomeRun at " + ip);
            return;
        }
        const FoundDevice &d = found.first();
        remember_ip(ip);   // re-added automatically after every scan

        // A greyed "not found" entry for this address can go now
        for (int i = ui->DeviceList->count() - 1; i >= 0; i--) {
            QListWidgetItem *it = ui->DeviceList->item(i);
            if (it->data(Qt::UserRole).toString() == ip && !(it->flags() & Qt::ItemIsEnabled)) {
                delete ui->DeviceList->takeItem(i);
            }
        }

        // Already listed (e.g. found by Scan)? Mark it remembered and select it.
        int first_match = -1;
        for (int i = 0; i < ui->DeviceList->count(); i++) {
            QListWidgetItem *it = ui->DeviceList->item(i);
            if (it->data(Qt::UserRole + 2).toString() == d.device_id) {
                it->setData(Qt::UserRole + 3, true);
                it->setToolTip(QString("%1 (remembered; right-click to forget)").arg(it->data(Qt::UserRole).toString()));
                if (first_match < 0) first_match = i;
            }
        }
        if (first_match >= 0) {
            ui->statusbar->showMessage(QString("Device %1 is already in the list; it'll now be remembered.").arg(d.device_id));
            ui->DeviceList->setCurrentRow(first_match);
            on_DeviceList_itemClicked(ui->DeviceList->item(first_match));
            return;
        }

        // Add its tuners below the ones already listed, and select the first
        int first_new = ui->DeviceList->count();
        add_device_tuners(d.ip, d.device_id, d.tuner_count, true, false);
        ui->statusbar->showMessage(QString("Added %1 (%2 tuners) at %3").arg(d.device_id).arg(d.tuner_count).arg(ip));
        if (ui->DeviceList->count() > first_new) {
            ui->DeviceList->setCurrentRow(first_new);
            on_DeviceList_itemClicked(ui->DeviceList->item(first_new));
        }
    });
}

void MainWindow::on_DeviceList_itemClicked(QListWidgetItem *item)
{
    on_StopButton_clicked();
    status_timer->stop();

    destroy_persistent_connection();

    current_device_ip = item->data(Qt::UserRole).toString();
    current_tuner_index = item->data(Qt::UserRole + 1).toInt();
    current_device_id = item->data(Qt::UserRole + 2).toString();

    ui->statusbar->showMessage(QString("Selected: %1-%2").arg(current_device_id).arg(current_tuner_index));

    create_persistent_connection();

    if (active_device) {
        populate_channel_maps();   // the maps this device supports
        QString full_map = read_tuner_var(active_device, current_tuner_index, "channelmap").trimmed();
        if (!full_map.isEmpty()) {
            ui->BroadcastMapBox->setCurrentText(full_map.split(" ").first());
        }

        clear_tuned_channel_info();
        set_wheel_silently(0);

        if (ui->A3SaveTypeBox) ui->A3SaveTypeBox->setCurrentIndex(1);
    }

    // Point the status thread at the new tuner and get a first reading now.
    point_poller_at_current_device();
    request_status_poll();
    status_timer->start(500);
}

// Fills the channel map box with the maps the device supports (from the
// "channelmap:" line of /sys/features), as the TUI does. Falls back to the
// four US maps if the device doesn't say.
void MainWindow::populate_channel_maps()
{
    QStringList maps;
    if (active_device) {
        char *val = NULL;
        if (hdhomerun_device_get_var(active_device, "/sys/features", &val, NULL) > 0 && val) {
            for (const QString &line : QString::fromUtf8(val).split('\n')) {
                if (line.startsWith("channelmap:")) {
                    maps = line.mid(11).split(' ', Qt::SkipEmptyParts);
                    break;
                }
            }
        }
    }
    if (maps.isEmpty()) maps = QStringList{"us-bcast", "us-cable", "us-hrc", "us-irc"};

    QString current = ui->BroadcastMapBox->currentText();
    ui->BroadcastMapBox->blockSignals(true);
    ui->BroadcastMapBox->clear();
    ui->BroadcastMapBox->addItems(maps);
    int idx = maps.indexOf(current);
    ui->BroadcastMapBox->setCurrentIndex(idx >= 0 ? idx : 0);
    ui->BroadcastMapBox->blockSignals(false);
    rebuild_channel_list(ui->BroadcastMapBox->currentText());
}

void MainWindow::on_BroadcastMapBox_currentTextChanged(const QString &arg1)
{
    rebuild_channel_list(arg1);
    if (!active_device) return;
    hdhomerun_device_set_tuner_channelmap(active_device, arg1.toStdString().c_str());
}

void MainWindow::on_ChannelWheel_valueChanged(int arg1)
{
    if (arg1 == 0) { wheel_last_value = 0; return; }

    // Not a channel in this map (below the first one, or a gap like broadcast
    // 37): carry on to the next real channel in the direction it was moving.
    // At the bottom end, snap back without retuning.
    if (!channel_numbers.contains(arg1)) {
        int dir = (arg1 >= wheel_last_value) ? 1 : -1;
        int target = next_valid_channel(arg1, dir, false);
        if (target < 0) target = next_valid_channel(arg1, -dir, false);
        if (target < 0 || target == wheel_last_value) {
            set_wheel_silently(wheel_last_value);
            return;
        }
        ui->ChannelWheel->setValue(target);   // comes back in here with a valid channel
        return;
    }
    wheel_last_value = arg1;

    on_StopButton_clicked();

    if (!active_device) return;

    QString channel_cmd = QString("auto:%1").arg(arg1);
    hdhomerun_device_set_tuner_channel(active_device, channel_cmd.toStdString().c_str());

    clear_tuned_channel_info();

    tune_generation++;      // discard any reading taken before this retune
    request_status_poll();
}

void MainWindow::on_ProgramBox_activated(int index) {
    if (index < 0) return;
    QString prog_num = ui->ProgramBox->itemData(index).toString();
    if (prog_num.isEmpty()) prog_num = program_number_from_line(ui->ProgramBox->itemText(index));
    if (prog_num.isEmpty()) return;

    if (!active_device) return;
    qDebug() << "Switching Program to:" << prog_num;

    // VLC doesn't follow a program change inside a running RTP stream (the
    // PAT/PMT change under it and the picture freezes). If we're viewing,
    // restart the view on the new program: on_ViewButton_clicked() stops the
    // old VLC, sets the program from ProgramBox, and opens a fresh one.
    if (vlc_process && vlc_process->state() != QProcess::NotRunning) {
        on_ViewButton_clicked();
        return;
    }
    hdhomerun_device_set_tuner_program(active_device, prog_num.toStdString().c_str());
}

void MainWindow::on_PLPList_itemChanged(QListWidgetItem *item)
{
    Q_UNUSED(item);   // we rebuild the PLP selection from the whole list
    if (!active_device) return;
    if (last_plp_freq.isEmpty()) return;

    QStringList selected_plps;
    for (int i = 0; i < ui->PLPList->count(); ++i) {
        QListWidgetItem *it = ui->PLPList->item(i);
        if (it->checkState() == Qt::Checked) {
            selected_plps.append(QString::number(it->data(Qt::UserRole).toInt()));
        }
    }

    if (!selected_plps.isEmpty()) {
        QString cmd = QString("atsc3:%1:%2").arg(last_plp_freq).arg(selected_plps.join("+"));
        qDebug() << "PLP Update:" << cmd;
        hdhomerun_device_set_tuner_channel(active_device, cmd.toStdString().c_str());
        tune_generation++;   // a reading from before this change would be stale
    }
}

void MainWindow::update_plp_ui(const TunerSnapshot &snap)
{
    if (!ui->PLPList) return;

    QString lock_str = QString::fromUtf8(snap.status.lock_str);
    QString channel_str = QString::fromUtf8(snap.status.channel);

    if (lock_str.contains("none")) return;

    if (!lock_str.contains("atsc3")) {
        if (!channel_str.startsWith("atsc3:")) {
            if (ui->PLPList->count() > 0) ui->PLPList->clear();
            last_plp_freq = "";
            if (ui->PLPCopyButton) ui->PLPCopyButton->setVisible(false);
        }
        return;
    }

    QStringList parts = channel_str.split(':');
    QString current_freq = (parts.size() >= 2) ? parts[1] : "";

    if (current_freq.isEmpty()) return;

    if (snap.plpinfo.isEmpty()) return;
    const QString &plp_data = snap.plpinfo;

    ui->PLPList->blockSignals(true);

    if (ui->PLPCopyButton) ui->PLPCopyButton->setVisible(true);

    bool specific_selection = (parts.size() >= 3);
    QList<int> active_plps;
    if (specific_selection) {
        QStringList plp_parts = parts[2].split('+');
        for (const QString &p : plp_parts) active_plps.append(p.toInt());
    }

    if (current_freq != last_plp_freq) {
        ui->PLPList->clear();
        last_plp_freq = current_freq;

        QStringList lines = plp_data.split('\n');
        for (const QString &line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;

            int colon_idx = trimmed.indexOf(':');
            if (colon_idx != -1) {
                bool ok;
                int plp_id = trimmed.left(colon_idx).toInt(&ok);
                if (ok) {
                    QListWidgetItem *item = new QListWidgetItem(trimmed, ui->PLPList);
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setData(Qt::UserRole, plp_id);
                    if (trimmed.contains("lock=1")) item->setForeground(kGoodColor);
                    else if (trimmed.contains("lock=0")) item->setForeground(kBadColor);

                    bool should_check = false;
                    if (specific_selection) {
                        should_check = active_plps.contains(plp_id);
                    } else {
                        should_check = trimmed.contains("lock=1");
                    }
                    item->setCheckState(should_check ? Qt::Checked : Qt::Unchecked);
                }
            }
        }
    }
    else {
        QStringList lines = plp_data.split('\n');
        QMap<int, QString> line_map;
        for(const QString &line : lines) {
            int colon_idx = line.indexOf(':');
            if (colon_idx != -1) {
                int id = line.left(colon_idx).toInt();
                line_map.insert(id, line.trimmed());
            }
        }

        for (int i = 0; i < ui->PLPList->count(); ++i) {
            QListWidgetItem *item = ui->PLPList->item(i);
            int id = item->data(Qt::UserRole).toInt();

            if (line_map.contains(id)) {
                QString new_text = line_map[id];
                item->setText(new_text);
                if (new_text.contains("lock=1")) item->setForeground(kGoodColor);
                else if (new_text.contains("lock=0")) item->setForeground(kBadColor);
                else item->setData(Qt::ForegroundRole, QVariant());

                if (!specific_selection) {
                    bool should_be_checked = new_text.contains("lock=1");
                    if (should_be_checked && item->checkState() == Qt::Unchecked) {
                        item->setCheckState(Qt::Checked);
                    } else if (!should_be_checked && item->checkState() == Qt::Checked) {
                        item->setCheckState(Qt::Unchecked);
                    }
                }
            }
        }
    }

    ui->PLPList->blockSignals(false);
}

QString MainWindow::get_local_ip_address() {
    QUdpSocket socket;
    socket.connectToHost(QHostAddress(current_device_ip), 65001);
    if (socket.waitForConnected(1000)) {
        QHostAddress addr = socket.localAddress();
        if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
            bool success;
            quint32 ipv4_int = addr.toIPv4Address(&success);
            if (success) return QHostAddress(ipv4_int).toString();
        }
        return addr.toString();
    }
    return "";
}

int MainWindow::get_free_port() {
    QUdpSocket socket;
    if (socket.bind(QHostAddress::AnyIPv4, 0)) {
        int port = socket.localPort();
        socket.close();
        return port;
    }
    return -1;
}

void MainWindow::on_ViewButton_clicked()
{
    if (current_device_ip.isEmpty()) return;
    on_StopButton_clicked();

    if (!active_device) create_persistent_connection();
    if (!active_device) return;

    struct hdhomerun_tuner_status_t status;
    char *raw_s;
    if (hdhomerun_device_get_tuner_status(active_device, &raw_s, &status) <= 0 || QString(status.lock_str) == "none") {
        ui->statusbar->showMessage("Cannot View: No Signal Lock.");
        return;
    }

    QString prog_num = ui->ProgramBox->currentData().toString();
    if (prog_num.isEmpty() && ui->ProgramBox->count() == 0) {
        // No list yet: use the first program the tuner reports
        QString info_str = read_tuner_var(active_device, current_tuner_index, "streaminfo");
        for (const QString &line : info_str.split('\n')) {
            prog_num = program_number_from_line(line);
            if (!prog_num.isEmpty()) break;
        }
    }

    if (!prog_num.isEmpty()) {
        hdhomerun_device_set_tuner_program(active_device, prog_num.toStdString().c_str());
    } else {
        ui->statusbar->showMessage("Error: No programs found.");
        return;
    }

    QString local_ip = get_local_ip_address();
    int port = get_free_port();
    if (local_ip.isEmpty() || port < 0) {
        ui->statusbar->showMessage("Network Error.");
        return;
    }

    QString vlc_url = QString("rtp://@:%1").arg(port);
    QStringList args;
    args << "--network-caching=1000";
    args << vlc_url;

    // 1. Start VLC. A fresh QProcess per view, so we never have to block
    //    waiting for the previous one to exit.
    vlc_process = new QProcess(this);
    connect(vlc_process, &QProcess::finished, this, &MainWindow::handle_vlc_finished);
    connect(vlc_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            stop_view();
            ui->statusbar->showMessage("Could not start VLC (is it installed and on PATH?).");
        }
    });
    vlc_process->start("vlc", args);
    ui->statusbar->showMessage("Starting VLC...");

    // 2./3. Give VLC 750 ms to open its socket, then set target and program --
    //       via a timer instead of sleeping on the GUI thread. If Stop was
    //       pressed meanwhile, view_generation has changed and we do nothing.
    int gen = view_generation;
    QString target = QString("rtp://%1:%2").arg(local_ip).arg(port);
    QTimer::singleShot(750, this, [this, gen, target, prog_num]() {
        if (gen != view_generation || !vlc_process || !active_device || is_saving) return;
        if (vlc_process->state() == QProcess::NotRunning) return;

        // Set Target FIRST (Important for proper PID filtering)
        QString target_path = QString("/tuner%1/target").arg(current_tuner_index);
        hdhomerun_device_set_var(active_device, target_path.toStdString().c_str(), target.toStdString().c_str(), NULL, NULL);

        // Set Program SECOND
        if (!prog_num.isEmpty()) {
            hdhomerun_device_set_tuner_program(active_device, prog_num.toStdString().c_str());
        }
        ui->statusbar->showMessage("Streaming to VLC...");
    });
}

// Stops VLC and the tuner's stream without blocking: VLC is asked to quit,
// killed 1.5 s later if it hasn't, and its QProcess deletes itself on exit.
void MainWindow::stop_view()
{
    view_generation++;   // cancels a pending "set target" step from View

    if (vlc_process) {
        QProcess *p = vlc_process;
        vlc_process = nullptr;
        p->disconnect(this);   // its exit must not call back into us
        if (p->state() != QProcess::NotRunning) {
            connect(p, &QProcess::finished, p, &QObject::deleteLater);
            p->terminate();
            QTimer::singleShot(1500, p, [p]() {
                if (p->state() != QProcess::NotRunning) p->kill();
            });
        } else {
            p->deleteLater();
        }
    }

    if (active_device) {
        QString target_path = QString("/tuner%1/target").arg(current_tuner_index);
        hdhomerun_device_set_var(active_device, target_path.toStdString().c_str(), "none", NULL, NULL);
    }
}

void MainWindow::on_StopButton_clicked()
{
    if (is_seeking) {
        seek_cancel_requested = true;
        return;
    }
    if (is_saving) {
        stop_current_save_operation();
        return;
    }

    stop_view();
    ui->statusbar->showMessage("Stream stopped.");
}

// The user closed VLC themselves.
void MainWindow::handle_vlc_finished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);
    if (is_saving) {
        // Don't stop a save (or touch its target) because VLC went away.
        if (vlc_process) { vlc_process->deleteLater(); vlc_process = nullptr; }
        return;
    }
    stop_view();
    ui->statusbar->showMessage("VLC closed; stream stopped.");
}

void MainWindow::on_SeekUpButton_clicked() { perform_seek(1); }
void MainWindow::on_SeekDownButton_clicked() { perform_seek(-1); }

void MainWindow::perform_seek(int direction) {
    if (ui_busy()) return;
    on_StopButton_clicked();
    if (!active_device) return;
    if (channel_numbers.isEmpty()) {
        ui->statusbar->showMessage("No channel list for this channel map; can't seek.");
        return;
    }

    // FIX #7: processEvents() below lets clicks through, so lock the UI for
    // the duration of the seek. Stop (button or Space) cancels it.
    is_seeking = true;
    seek_cancel_requested = false;
    set_ui_enabled(false);
    ui->SaveButton->setEnabled(false);

    status_timer->stop();
    clear_tuned_channel_info();   // blank programs/IDs/PLPs as soon as we leave the channel
    ui->ChannelInfo->setText("Scanning...");

    // Step through the channels of the current map, wrapping at either end
    int current_channel = ui->ChannelWheel->value();
    if (current_channel == 0) {
        current_channel = (direction > 0) ? channel_numbers.first() : channel_numbers.last();
    } else {
        current_channel = next_valid_channel(current_channel, direction, true);
    }
    int start_channel = current_channel;

    bool first_loop = true;
    bool cancelled = false;

    while (true) {
        if (seek_cancel_requested || !active_device) { cancelled = true; break; }

        if (!first_loop) {
            current_channel = next_valid_channel(current_channel, direction, true);
        }

        if (!first_loop && current_channel == start_channel) {
            ui->statusbar->showMessage("Seek complete: No signals found.");
            break;
        }
        first_loop = false;

        QString cmd = QString("auto:%1").arg(current_channel);
        ui->statusbar->showMessage(QString("Seeking... Checking %1").arg(current_channel));
        hdhomerun_device_set_tuner_channel(active_device, cmd.toStdString().c_str());

        set_wheel_silently(current_channel);

        bool locked = false;
        for (int i = 0; i < 25; i++) {
            if (seek_cancel_requested || !active_device) break;

            struct hdhomerun_tuner_status_t status;
            char *raw_s = NULL;
            if (hdhomerun_device_get_tuner_status(active_device, &raw_s, &status) > 0) {

                long rssi = parse_db_value(raw_s, "ss=");
                long snr = parse_db_value(raw_s, "snq=");

                colorize_bar(ui->BarStrength, status.signal_strength, rssi, "dBm");
                ui->BarStrength->setValue(status.signal_strength);

                colorize_bar(ui->BarSigQuality, status.signal_to_noise_quality, snr, "dB");
                ui->BarSigQuality->setValue(status.signal_to_noise_quality);

                colorize_bar(ui->BarSymQuality, status.symbol_error_quality, -999, "");
                ui->BarSymQuality->setValue(status.symbol_error_quality);

                QString full_channel_str = QString("%1 (%2)")
                                               .arg(QString::fromUtf8(status.channel))
                                               .arg(QString::fromUtf8(status.lock_str));
                ui->ChannelInfo->setText(full_channel_str);

                if (status.signal_to_noise_quality > 0) {
                    locked = true;
                    break;
                }
            }
            QApplication::processEvents();
            QThread::msleep(100);
        }

        if (locked) {
            ui->statusbar->showMessage(QString("Locked on channel %1").arg(current_channel));
            break;
        }
    }

    if (cancelled) ui->statusbar->showMessage("Seek cancelled.");

    is_seeking = false;
    seek_cancel_requested = false;
    tune_generation++;   // any reading from before the seek is stale
    set_ui_enabled(true);
    ui->SaveButton->setEnabled(true);
    status_timer->start(500);
}

// status_timer tick: ask the poller thread for a reading (never blocks).
// Only one request is outstanding at a time, so a slow device can't make
// requests pile up.
void MainWindow::request_status_poll()
{
    if (poll_in_flight || is_seeking || !poller) return;
    poll_in_flight = true;
    poll_tune_generation = tune_generation;
    QMetaObject::invokeMethod(poller, "poll", Qt::QueuedConnection);
}

void MainWindow::point_poller_at_current_device()
{
    device_generation++;
    QString dev = device_connect_string();
    int tuner = current_tuner_index;
    int gen = device_generation;
    StatusPoller *p = poller;
    QMetaObject::invokeMethod(poller, [p, dev, tuner, gen]() { p->set_device(dev, tuner, gen); },
                              Qt::QueuedConnection);
}

// Poller result: same display logic as the old update_signal_status(), but
// working only from strings the poller already copied -- no device calls here.
void MainWindow::handle_status_snapshot(TunerSnapshot snap)
{
    poll_in_flight = false;

    // Drop readings for a previous tuner, from before a retune, or mid-seek
    if (snap.generation != device_generation) return;
    if (poll_tune_generation != tune_generation) return;
    if (is_seeking) return;

    last_snapshot = snap;
    if (!snap.ok) return;

    const struct hdhomerun_tuner_status_t &status = snap.status;
    QByteArray raw = snap.raw_status.toUtf8();
    long rssi = parse_db_value(raw.constData(), "ss=");
    long snr = parse_db_value(raw.constData(), "snq=");
    long status_pps = parse_status_value(raw.constData(), "pps=");

    colorize_bar(ui->BarStrength, status.signal_strength, rssi, "dBm");
    ui->BarStrength->setValue(status.signal_strength);
    colorize_bar(ui->BarSigQuality, status.signal_to_noise_quality, snr, "dB");
    ui->BarSigQuality->setValue(status.signal_to_noise_quality);
    colorize_bar(ui->BarSymQuality, status.symbol_error_quality, -999, "");
    ui->BarSymQuality->setValue(status.symbol_error_quality);

    QString lock_str = QString::fromUtf8(status.lock_str);
    bool locked = (lock_str != "none");

    if (locked) {
        QString full_channel_str = QString("%1 (%2)")
                                       .arg(QString::fromUtf8(status.channel))
                                       .arg(lock_str);
        ui->ChannelInfo->setText(full_channel_str);
    }

    QString ch_str = QString::fromUtf8(status.channel).trimmed();
    QStringList parts = ch_str.split(':');
    int ch_num = -1;
    bool ok;
    for (const QString &part : parts) {
        int val = part.toInt(&ok);
        if (ok && channel_numbers.contains(val)) { ch_num = val; break; }
        long freq = part.toLong(&ok);
        if (ok && freq > 1000000) { ch_num = frequency_to_channel(freq); break; }
    }
    if (ch_num == -1 && ch_str.startsWith("ch", Qt::CaseInsensitive)) {
        int val = ch_str.mid(2).toInt(&ok);
        if (ok) ch_num = val;
    }

    if (locked && ch_num > 0 && ch_num != ui->ChannelWheel->value()) {
        set_wheel_silently(ch_num);
    }

    bool is_atsc3 = lock_str.contains("atsc3");
    update_stream_ids(snap, is_atsc3);

    long bps = -999;
    long pps = -999;
    if (!snap.debug.isEmpty()) {
        QByteArray dbg = snap.debug.toUtf8();
        const char *flt_ptr = strstr(dbg.constData(), "flt:");
        if (flt_ptr) bps = parse_status_value(flt_ptr, "bps=");
        const char *net_ptr = strstr(dbg.constData(), "net:");
        if (net_ptr) pps = parse_status_value(net_ptr, "pps=");
    }
    if (bps == -999 && pps == -999) pps = status_pps;

    double final_rate_mbps = 0.0;
    if (bps > 0) final_rate_mbps = (double)bps / 1000000.0;
    else if (pps > 0) final_rate_mbps = (double)(pps * 1358 * 8) / 1000000.0;

    if (final_rate_mbps > 0.001) ui->NetworkRate->setText(QString::number(final_rate_mbps, 'f', 3) + " Mbps");
    else ui->NetworkRate->setText("0.000 Mbps");

    if (!snap.target.isEmpty()) ui->NetworkTarget->setText(snap.target);

    update_plp_ui(snap);
    if (locked) {
        update_programs_list(snap.streaminfo);
    } else {
        ui->ProgramBox->clear();
        // Forget the cache, or the list stays empty if the same mux relocks
        ui->ProgramBox->setProperty("last_streaminfo", QString());
    }

    bool is_tech_model = (rssi != -999);
    bool show_l1 = is_atsc3 && is_tech_model;

    if (ui->L1CopyButton && ui->L1InfoButton) {
        ui->L1CopyButton->setVisible(show_l1);
        ui->L1InfoButton->setVisible(show_l1);

        if (ui->A3SaveTypeBox && ui->A3SaveTypeLabel) {
            ui->A3SaveTypeBox->setVisible(show_l1);
            ui->A3SaveTypeLabel->setVisible(show_l1);
        }

        if (show_l1 && !snap.l1detail.isEmpty()) {
            current_l1_string = snap.l1detail.trimmed();
        }
    }
}

// Blank everything that belongs to the channel we're tuning away from.
void MainWindow::clear_tuned_channel_info()
{
    ui->ChannelInfo->clear();
    current_l1_string.clear();
    ui->TSIDBox->setText("Not Set");
    if (ui->SLTBSIDBox) {
        ui->SLTBSIDBox->setText("Not Set");
        ui->SLTBSIDBox->setStyleSheet(QString());
        ui->TSIDBox->setStyleSheet(QString());
    }

    ui->ProgramBox->blockSignals(true);
    ui->ProgramBox->clear();
    ui->ProgramBox->blockSignals(false);
    ui->ProgramBox->setProperty("last_streaminfo", QString());

    last_plp_freq = "";
    if (ui->PLPList) ui->PLPList->clear();
    if (ui->PLPCopyButton) ui->PLPCopyButton->setVisible(false);
}

// ATSC 1.0: one "TSID" box across the row.
// ATSC 3.0: the row splits into "L1D BSID" (plpinfo bsid=, as reported from
// L1-Detail) and "SLT BSID" (streaminfo tsid=, from the LLS SLT) -- the same
// sources the L1 detail parser uses. A mismatch is highlighted, as in RENDER.
void MainWindow::update_stream_ids(const TunerSnapshot &snap, bool is_atsc3)
{
    auto fmt = [](long v) { return QString("%1 (0x%2)").arg(v).arg(v, 4, 16, QChar('0')); };
    auto parse = [this](const QString &src, const char *key) -> long {
        if (src.isEmpty()) return -999;
        QByteArray b = src.toUtf8();
        return parse_status_value(b.constData(), key);
    };

    ui->LabelTSID->setText(is_atsc3 ? "L1D BSID" : "TSID");
    if (ui->LabelSLTBSID) ui->LabelSLTBSID->setVisible(is_atsc3);
    if (ui->SLTBSIDBox) ui->SLTBSIDBox->setVisible(is_atsc3);

    if (!is_atsc3) {
        ui->TSIDBox->setStyleSheet(QString());
        long tsid = parse(snap.streaminfo, "tsid=");
        if (tsid != -999) ui->TSIDBox->setText(fmt(tsid));
        return;
    }

    long l1d = parse(snap.plpinfo, "bsid=");
    long slt = parse(snap.streaminfo, "tsid=");
    if (l1d != -999) ui->TSIDBox->setText(fmt(l1d));
    if (slt != -999 && ui->SLTBSIDBox) ui->SLTBSIDBox->setText(fmt(slt));

    bool mismatch = (l1d != -999 && slt != -999 && l1d != slt);
    QString warn = mismatch ? "QLineEdit { background: #fff59d; color: black; }" : QString();
    QString tip = mismatch ? "L1D BSID and SLT BSID differ" : QString();
    ui->TSIDBox->setStyleSheet(warn);
    ui->TSIDBox->setToolTip(tip);
    if (ui->SLTBSIDBox) {
        ui->SLTBSIDBox->setStyleSheet(warn);
        ui->SLTBSIDBox->setToolTip(tip);
    }
}

// Program number from one streaminfo line, matching the TUI's parser:
//   ATSC 1.0  "3: 4.1 WABC-HD"  or  "program 3: ..."   -> "3"
//   ATSC 3.0  "program=5001 vchannel=... plp=0"         -> "5001"
// Anything else (tsid=, bsid=, blank) -> empty.
QString MainWindow::program_number_from_line(const QString &line)
{
    QString t = line.trimmed();
    if (t.startsWith("program=")) return t.mid(8).section(' ', 0, 0).trimmed();
    if (t.startsWith("program ")) return t.mid(8).section(' ', 0, 0).section(':', 0, 0).trimmed();
    if (!t.isEmpty() && t[0].isDigit() && t.contains(':')) return t.section(' ', 0, 0).section(':', 0, 0).trimmed();
    return QString();
}

void MainWindow::update_programs_list(const QString &info_str)
{
    if (info_str.isEmpty()) return;
    if (ui->ProgramBox->property("last_streaminfo").toString() == info_str) return;

    QString current_selection = ui->ProgramBox->currentText();
    ui->ProgramBox->blockSignals(true);
    ui->ProgramBox->clear();

    QStringList lines = info_str.split('\n');
    for (const QString &line : lines) {
        QString number = program_number_from_line(line);
        if (!number.isEmpty()) {
            ui->ProgramBox->addItem(line.trimmed(), number);   // number kept as item data
            if (line.contains("(encrypted)")) {   // red, as in the TUI
                ui->ProgramBox->setItemData(ui->ProgramBox->count() - 1, kBadColor, Qt::ForegroundRole);
            }
        }
    }

    int idx = ui->ProgramBox->findText(current_selection);
    if (idx != -1) ui->ProgramBox->setCurrentIndex(idx);

    ui->ProgramBox->setProperty("last_streaminfo", info_str);
    ui->ProgramBox->blockSignals(false);
}

void MainWindow::on_PLPCopyButton_clicked() {
    if (!active_device) return;
    QString plpinfo = read_tuner_var(active_device, current_tuner_index, "plpinfo");
    if (plpinfo.isEmpty()) {
        ui->statusbar->showMessage("Couldn't read PLP info from the tuner.");
        return;
    }
    QGuiApplication::clipboard()->setText(plpinfo);
    ui->statusbar->showMessage("PLP Info copied to clipboard.");
}
void MainWindow::on_L1CopyButton_clicked() {
    if (current_l1_string.isEmpty()) {
        ui->statusbar->showMessage("No L1 string from the tuner yet.");
        return;
    }
    QGuiApplication::clipboard()->setText(current_l1_string);
    ui->statusbar->showMessage("L1 String copied to clipboard.");
}
void MainWindow::on_L1InfoButton_clicked() {
    if (!active_device) {
        QMessageBox::warning(this, "L1 Detail Info", "Not connected to a tuner.");
        return;
    }

    // 1. Collect Data
    struct l1_detail_info* detail_info = create_l1_detail_info(MAX_DISPLAY_LINES);
    if (!detail_info) {
        QMessageBox::warning(this, "L1 Detail Info", "Out of memory.");
        return;
    }

    // Use a fresh connection for this, like the status and save threads do.
    struct hdhomerun_device_t *l1_dev =
        hdhomerun_device_create_from_str(device_connect_string().toUtf8().constData(), NULL);
    if (!l1_dev) {
        free_l1_detail_info(detail_info);
        QMessageBox::warning(this, "L1 Detail Info", "Couldn't open a connection to the tuner.");
        return;
    }
    hdhomerun_device_set_tuner(l1_dev, current_tuner_index);
    struct tuner t = { l1_dev, current_tuner_index };

    int rc = collect_atsc3_details(&t, detail_info);
    hdhomerun_device_destroy(l1_dev);
    l1_dev = nullptr;
    if (rc != 0 || detail_info->line_count == 0) {
        free_l1_detail_info(detail_info);
        QMessageBox::warning(this, "L1 Detail Info",
                             rc != 0 ? "The tuner didn't return any PLP info. Is it locked to an ATSC 3.0 signal?"
                                     : "The tuner returned no L1 details.");
        return;
    }

    QString display_text;
    for (int i = 0; i < detail_info->line_count; i++) {
        QString line = QString::fromUtf8(detail_info->display_lines[i]);
        if (line == "__HLINE__") display_text += "--------------------------------------------------\n";
        else display_text += line + "\n";
    }
    free_l1_detail_info(detail_info);

    // 2. Setup Dialog UI
    QDialog *dialog = new QDialog(this);
    dialog->setWindowTitle("L1 Detail Info");
    dialog->resize(600, 700);

    QVBoxLayout *layout = new QVBoxLayout(dialog);

    QTextEdit *textBox = new QTextEdit(dialog);
    textBox->setReadOnly(true);
    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    textBox->setFont(fixedFont);
    textBox->setText(display_text);
    layout->addWidget(textBox);

    QHBoxLayout *btnLayout = new QHBoxLayout();

    QPushButton *btnCopy = new QPushButton("Copy to Clipboard", dialog);
    QPushButton *btnSave = new QPushButton("Save to File", dialog);
    QPushButton *btnClose = new QPushButton("Close", dialog);

    btnLayout->addWidget(btnCopy);
    btnLayout->addWidget(btnSave);
    btnLayout->addStretch();
    btnLayout->addWidget(btnClose);
    layout->addLayout(btnLayout);

    connect(btnCopy, &QPushButton::clicked, btnCopy, [textBox, btnCopy]() {
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(textBox->toPlainText());
        btnCopy->setText("Copied!");
        QTimer::singleShot(1000, btnCopy, [btnCopy](){ btnCopy->setText("Copy to Clipboard"); });
    });

    connect(btnSave, &QPushButton::clicked, [textBox, this, dialog]() {
        QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        QString suggestName = QString("L1-Detail-%1.txt").arg(timestamp);
        QString initialPath = QDir(save_directory).filePath(suggestName);

        QString fileName = QFileDialog::getSaveFileName(dialog, "Save L1 Details", initialPath, "Text Files (*.txt);;All Files (*)");

        if (!fileName.isEmpty()) {
            QFile file(fileName);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&file);
                out << textBox->toPlainText();
                file.close();
            }
        }
    });

    connect(btnClose, &QPushButton::clicked, dialog, &QDialog::accept);

    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
