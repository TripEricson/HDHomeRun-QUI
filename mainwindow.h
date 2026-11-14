/*
 * mainwindow.h
 *
 * Main window, status poller and capture worker declarations.
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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidgetItem>
#include <QTimer>
#include <QList>
#include <QProcess>
#include <QUdpSocket>
#include <QInputDialog>
#include <QClipboard>
#include <QDialog>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QFontDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QThread>
#include <QDateTime>
#include <QSettings>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QCloseEvent>
#include <QVector>
#include <functional>
#include <atomic>

extern "C" {
#include <hdhomerun.h>
}

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// --- One poll's worth of tuner data, copied out of libhdhomerun's buffers ---
struct TunerSnapshot {
    int generation = 0;          // matches MainWindow::device_generation when current
    bool ok = false;             // tuner status read succeeded
    struct hdhomerun_tuner_status_t status;
    QString raw_status;
    QString plpinfo;             // ATSC 3.0 only
    QString streaminfo;          // when locked
    QString debug;
    QString target;
    QString l1detail;            // ATSC 3.0 on models that report dBm
    qint64 timestamp_ms = 0;
    TunerSnapshot() { memset(&status, 0, sizeof(status)); }
};
Q_DECLARE_METATYPE(TunerSnapshot)

// --- Status poller: runs in its own thread with its own device handle, so a
// slow or unreachable device never freezes the GUI. ---
class StatusPoller : public QObject {
    Q_OBJECT
public:
    explicit StatusPoller(QObject *parent = nullptr) : QObject(parent) {}
    ~StatusPoller();

public slots:
    void set_device(QString dev_str, int tuner, int generation);
    void poll();

signals:
    void snapshot_ready(TunerSnapshot snap);

private:
    struct hdhomerun_device_t *device = nullptr;
    int tuner_index = 0;
    int current_generation = 0;
};

// --- Worker for ATSC 1.0 Capture (Threaded) ---
class SaveWorker : public QObject {
    Q_OBJECT
public:
    // dev_str is the device IP (or device ID); tuner_idx selects the tuner.
    // The worker opens its OWN device handle so it never shares a control
    // socket with the GUI thread.
    explicit SaveWorker(QString dev_str, QString filename, int duration, bool restart_on_err, int tuner_idx, QObject *parent = nullptr);
    ~SaveWorker();
    void request_stop();

public slots:
    void process();

signals:
    void finished(QString message);
    void error_detected(QString filename); // Signal to main thread to handle restart
    void progress(int seconds_remaining);

private:
    QString device_str;
    QString file_name;
    int duration_sec;
    bool restart_enabled;
    int tuner_index;
    std::atomic<bool> stop_requested;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void on_ScanButton_clicked();
    void on_DeviceList_itemClicked(QListWidgetItem *item);
    void on_BroadcastMapBox_currentTextChanged(const QString &arg1);
    void on_ChannelWheel_valueChanged(int arg1);

    void on_SeekUpButton_clicked();
    void on_SeekDownButton_clicked();

    void on_ViewButton_clicked();
    void on_StopButton_clicked();
    void handle_vlc_finished(int exitCode, QProcess::ExitStatus exitStatus);

    void on_ProgramBox_activated(int index);
    void on_PLPList_itemChanged(QListWidgetItem *item);

    void on_IPButton_clicked();

    // Copy Buttons
    void on_PLPCopyButton_clicked();
    void on_L1CopyButton_clicked();

    // L1 Info Button
    void on_L1InfoButton_clicked();

    // Save Feature Slots
    void on_SaveButton_clicked();
    void on_SetFolderButton_clicked();

    // Renamed slots to avoid Qt auto-connect warnings
    void handle_atsc3_save_finished();
    void handle_atsc3_readyRead();
    void check_atsc3_status();

    void handle_atsc1_worker_finished(QString msg);
    void handle_atsc1_worker_error(QString filename);
    void handle_atsc1_worker_progress(int remaining);

    void request_status_poll();                       // status_timer tick
    void handle_status_snapshot(TunerSnapshot snap);  // poller result

private:
    Ui::MainWindow *ui;
    QTimer *status_timer;
    QProcess *vlc_process;        // created per View; nullptr when not viewing
    int view_generation = 0;      // invalidates delayed View steps after Stop

    // Status polling thread
    QThread *poller_thread;
    StatusPoller *poller;
    int device_generation = 0;   // bumped when the tuner/device changes
    int tune_generation = 0;     // bumped on every retune from the GUI
    int poll_tune_generation = 0;
    bool poll_in_flight = false;
    TunerSnapshot last_snapshot;
    QString current_l1_string;   // latest raw L1 string, for Copy L1 String

    // ATSC 3.0 save health check
    qint64 atsc3_save_started_ms = 0;
    bool atsc3_seen_good = false;

    // Save countdown (ATSC 3.0 uses this timer; ATSC 1.0 uses worker progress)
    QTimer *countdown_timer;
    qint64 countdown_end_ms = 0;
    void show_save_remaining(int seconds);
    void update_atsc3_countdown();

    // L1 sidecar (.txt next to each ATSC 3.0 capture)
    QString l1_sidecar_filename() const;
    void write_l1_sidecar();
    void remove_capture_files();   // capture + its .txt

    struct hdhomerun_device_t* active_device;
    QString current_device_id;
    QString current_device_ip;
    int current_tuner_index;

    QString last_plp_freq;

    // Networking for ATSC 3.0 Save
    QNetworkAccessManager *nam;
    QNetworkReply *current_reply;
    QFile *save_file;
    QTimer *atsc3_monitor_timer;

    // Stoppable Restart Timer
    QTimer *restart_timer;

    // Threading for ATSC 1.0 Save
    QThread *worker_thread;
    SaveWorker *save_worker;

    // Seek State
    bool is_seeking;
    bool seek_cancel_requested;

    // Save State
    bool is_saving;
    bool save_is_atsc3;   // which path a save (and its auto-restarts) uses
    int save_attempts;
    int plp_lock_checks = 0;   // 3.0 save: checks so far waiting for PLPs to lock
    QString current_save_filename;
    QString save_directory;

    // CACHE VARIABLES
    QString saved_original_channel; // Stores the full channel string for restoration
    QString cached_plp_str;
    QString cached_format;
    bool cached_is_pcap;
    int cached_duration;

    // Helpers
    void create_persistent_connection();
    void destroy_persistent_connection();

    struct hdhomerun_device_t* get_device_handle();
    QString device_connect_string() const;   // device IP, falls back to device ID

    // Channel map, from libhdhomerun's own tables, sorted by channel number.
    // Drives the channel spinner, seek, and frequency -> channel conversion.
    QVector<int> channel_numbers;
    QVector<long> channel_freqs;          // centre frequency for each entry
    int wheel_last_value = 0;             // last value the spinner settled on
    void rebuild_channel_list(const QString &channelmap);
    void populate_channel_maps();   // maps the device supports, from /sys/features
    int next_valid_channel(int from, int direction, bool wrap) const;
    void set_wheel_silently(int value);

    // Asks the device at ip for its ID and real tuner count. false if unreachable.
    // Static and UI-free, so it can run on the background scan thread.
    static bool detect_device(const QString &ip, QString *device_id_out, int *tuner_count_out);
    // Device list items: UserRole = IP, +1 = tuner, +2 = device ID,
    // +3 = remembered (added by IP), +4 = found by Scan's broadcast discovery
    void add_device_tuners(const QString &ip, const QString &device_id, int tuner_count,
                           bool remembered = false, bool discovered = true);
    void add_unreachable_device(const QString &ip);   // greyed "not found" entry
    void select_first_tuner();                        // first enabled entry, if any

    // Addresses added with Enter IP, kept between runs and re-added after
    // every scan. Right-click one in the device list to forget it.
    QStringList remembered_ips() const;
    void set_remembered_ips(const QStringList &ips);
    void remember_ip(const QString &ip);
    void forget_remembered_ip(const QString &ip);
    void show_device_menu(const QPoint &pos);

    // Program number from a streaminfo line, in any of the formats the TUI
    // understands: "3: 4.1 NAME", "program 3: ...", "program=5001 ...".
    static QString program_number_from_line(const QString &line);

    // Background device discovery (Scan, Enter IP), so the window never freezes
    struct FoundDevice {
        QString ip;
        QString device_id;
        int tuner_count = 0;          // 0 = a remembered address that didn't answer
        bool remembered = false;
        bool discovered = false;      // found by broadcast discovery
    };
    static QList<FoundDevice> discover_all_devices(const QStringList &remembered_ips);   // runs on scan_thread
    QThread *scan_thread = nullptr;
    bool is_scanning = false;
    void run_scan_in_background(std::function<QList<FoundDevice>()> job, const QString &busy_msg,
                                std::function<void(const QList<FoundDevice> &)> done);
    void forget_current_device(bool clear_list = true);   // drop connections (+ the device list)
    bool ui_busy() const { return is_saving || is_seeking || is_scanning; }
    void cleanup_atsc1_worker();

    long parse_status_value(const char *status_str, const char *key);
    long parse_db_value(const char *status_str, const char *key);
    long get_stream_id_value(struct hdhomerun_device_t* device, bool is_atsc3);
    int frequency_to_channel(long freq);

    void update_programs_list(const QString &streaminfo);
    void update_plp_ui(const TunerSnapshot &snap);
    void update_stream_ids(const TunerSnapshot &snap, bool is_atsc3);
    void clear_tuned_channel_info();   // blank programs/IDs/PLPs on retune
    void point_poller_at_current_device();   // (re)targets the poller thread
    void stop_view();                        // stop VLC + clear tuner target, non-blocking
    QString release_tuner_from_http();       // frees our HTTP stream's lock; returns another holder's address

    void colorize_bar(class QProgressBar* bar, int value, long db_val, QString unit);
    void perform_seek(int direction);

    void set_ui_enabled(bool enabled);

    void restore_tuner_state(); // NEW: Mimics TUI restore_and_exit
    void perform_auto_restart();
    void teardown_atsc3_transfer();
    void stop_current_save_operation();
    void start_atsc3_save();
    void start_atsc1_save();

    QString get_local_ip_address();
    int get_free_port();
};
#endif // MAINWINDOW_H
