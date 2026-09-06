// SPDX-License-Identifier: BSD-3-Clause
//
// Session state-machine tests. A QTcpServer on localhost plays the receiver and
// speaks the real ASCII protocol (p.61-64), so the bytes travel through the
// vendored SsnRx parser exactly as they do on hardware. That is the only way to
// cover reply<->command binding, listing completion and the stray prompts SsnRx
// generates on its own.
//
// Timer-driven paths (15 s command timeout, 30 s SBF watchdog, 5 s backoff) are
// deliberately not covered: their constants are not injectable and wall-clock
// waits do not belong in a unit test.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <doctest/doctest.h>

#include "sbf_frame_builder.h"
#include "session.h"

extern "C" {
#include "crc.h"
}

namespace {
    // ---------------------------------------------------------------- helpers

    std::filesystem::path Scratch(const std::string &name) {
        const auto p = std::filesystem::temp_directory_path() /
                       ("asterx_session_" + std::to_string(::getpid()) + "_" + name);
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }

    // Run the event loop until `done` or the deadline
    bool SpinUntil(const std::function<bool()> &done, int timeout_ms = 5000) {
        QElapsedTimer clock;
        clock.start();
        while (!done() && clock.elapsed() < timeout_ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return done();
    }

    // Run the event loop for a fixed time (to prove something does NOT happen)
    void Pump(int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
    }

    // One complete ReceiverStatus frame with a CRC the SsnRx parser accepts
    std::vector<std::uint8_t> ReceiverStatusFrame(std::uint32_t up_time, std::uint32_t rx_status) {
        asterx_test::FrameBuilder fb;
        asterx_test::rxs_fixed_body(fb, /*n=*/0, /*sb_length=*/0, up_time, rx_status);
        auto f = fb.finalize(4014, 0);
        const auto crc = CRC_compute16CCITT(f.data() + 4, f.size() - 4);
        f[2] = static_cast<std::uint8_t>(crc & 0xFF);
        f[3] = static_cast<std::uint8_t>(crc >> 8);
        return f;
    }

    QStringList SplitArgs(const QString &command) {
        QStringList out;
        for (const auto &part: command.split(',')) {
            out << part.trimmed();
        }
        return out;
    }

    // ---------------------------------------------------------- fake receiver

    // Speaks enough of the command-line interface for the configure sequence:
    // "$R:" echo replies for set/get/exe, "$R!" for login, "$R;" + formatted
    // blocks for lst, and the prompt that terminates each of them.
    class FakeReceiver {
    public:
        explicit FakeReceiver(QString descriptor = "IP10") : cd_(std::move(descriptor)) {
            server_.listen(QHostAddress::LocalHost, 0);
            QObject::connect(&server_, &QTcpServer::newConnection, &ctx_, [this] {
                socket_ = server_.nextPendingConnection();
                QObject::connect(socket_, &QTcpSocket::readyRead, &ctx_, [this] { OnRead(); });
            });
        }

        quint16 Port() const { return server_.serverPort(); }
        const QStringList &Commands() const { return commands_; }

        bool Saw(const QString &command) const { return commands_.contains(command); }

        int IndexOf(const QString &command) const { return commands_.indexOf(command); }

        // Test hooks
        QString reject_command; // answer "$R?" to this command name
        QStringList extra_config_lines; // injected into the lstConfigFile listing
        QString capabilities_antennas{"Main+Aux1"};
        QString stray_prompt_before; // send a bare prompt before answering this command name
        QString wrong_echo_before; // answer this command once with a foreign echo first
        std::function<void(const QString &)> after_command;

        void SendSbf(const std::vector<std::uint8_t> &frame) {
            if (socket_) {
                socket_->write(reinterpret_cast<const char *>(frame.data()),
                               static_cast<qint64>(frame.size()));
                socket_->flush();
            }
        }

    private:
        static bool IsIndexed(const QString &key) {
            // Commands whose get-counterpart takes the same index argument (p.63)
            static const QStringList indexed{
                "DataInOut", "AntennaType", "NMEAOutput", "SBFOutput", "NtripSettings", "NtripTlsSettings"
            };
            return indexed.contains(key);
        }

        QString Prompt() const { return cd_ + ">"; }

        void Send(const QString &text) {
            if (!socket_) {
                return;
            }
            const QByteArray bytes = text.toUtf8();
            socket_->write(bytes);
            socket_->flush();
        }

        void OnRead() {
            buffer_ += socket_->readAll();
            int end;
            while ((end = buffer_.indexOf("\r\n")) >= 0) {
                const QString line = QString::fromUtf8(buffer_.left(end));
                buffer_ = buffer_.mid(end + 2);
                Handle(line.trimmed());
            }
        }

        void Handle(const QString &command) {
            if (command.isEmpty() || command == "SSSSSSSSSS") {
                Send(Prompt()); // an empty command is answered with the Prompt (p.62)
                return;
            }
            commands_ << command;
            const QStringList args = SplitArgs(command);
            const QString name = args.value(0);

            if (!stray_prompt_before.isEmpty() && name == stray_prompt_before) {
                // What SsnRx's own 5 s keep-alive ("\r\n") provokes: a bare
                // prompt that arrives before the real reply.
                Send(Prompt());
            }
            if (!reject_command.isEmpty() && name == reject_command) {
                Send("$R? " + name + ": Not authorized!\r\n" + Prompt());
                return;
            }
            if (!wrong_echo_before.isEmpty() && name == wrong_echo_before) {
                wrong_echo_before.clear();
                // Same payload, but the echo names a different command: this is
                // what a one-command-late reply looks like on the wire.
                Send("$R: getNTPServer\r\n  " + PayloadFor(args) + "\r\n" + Prompt());
            }

            if (name == "login") {
                Send("$R! LogIn\r\n  User " + args.value(1) + " logged in.\r\n" + Prompt());
            } else if (name == "exeCopyConfigFile") {
                store_.clear();
                config_lines_.clear();
                Send("$R: " + command + "\r\n  CopyConfigFile, " + args.mid(1).join(", ") + "\r\n" + Prompt());
            } else if (name == "getReceiverCapabilities") {
                Send("$R: " + command + "\r\n  ReceiverCapabilities, " + capabilities_antennas +
                     ", GPSL1CA+GPSL5, COM1+IPS1,\r\n      APME+INS, 5, 100, 5\r\n" + Prompt());
            } else if (name == "lstAntennaInfo") {
                SendListing(command,
                             "<?xml version=\"1.0\" encoding=\"ISO-8859-1\" ?>\r\n"
                             "<AntennaInfo version=\"22.1.0\">\r\n"
                             "  <Antenna ID=\"AS-ANT2B        NONE\"/>\r\n"
                             "  <Antenna ID=\"AERAT2775_159   SPKE\"/>\r\n"
                             "</AntennaInfo>");
            } else if (name == "lstConfigFile") {
                QStringList lines{"  #setIPSettings, Static, 10.95.2.102, 255.255.255.0, 10.95.2.1",
                                  "  setUserAccessLevel, User1, \"admin\", \"Xy12ab\", User"};
                for (const auto &l: config_lines_) {
                    lines << "  " + l;
                }
                for (const auto &l: extra_config_lines) {
                    lines << "  " + l;
                }
                SendListing(command, lines.join("\r\n"));
            } else if (name.startsWith("set")) {
                const QString key = name.mid(3);
                const QString index = IsIndexed(key) ? args.value(1) : QString();
                store_.insert(key + "|" + index, StoredValue(key, args));
                config_lines_ << command;
                Send("$R: " + command + "\r\n  " + PayloadFor(args) + "\r\n" + Prompt());
            } else if (name.startsWith("get")) {
                const QString key = name.mid(3);
                const QString index = args.size() > 1 ? args.value(1) : QString();
                const QString value = store_.value(key + "|" + index);
                if (value.isEmpty()) {
                    Send("$R? " + name + ": nothing configured\r\n" + Prompt());
                } else {
                    Send("$R: " + command + "\r\n  " + key + ", " + value + "\r\n" + Prompt());
                }
            } else {
                Send("$R? " + name + ": unknown command\r\n" + Prompt());
            }

            if (after_command) {
                after_command(command);
            }
        }

        // What the receiver reports back for a set command it just executed
        static QString StoredValue(const QString &key, const QStringList &args) {
            if (key == "DataInOut") {
                // "When opening an IPxx connection, the Input and Output modes are
                // always reset to their default value" (p.169): auto / SBF+NMEA.
                return args.value(1) + ", auto, SBF+NMEA";
            }
            return args.mid(1).join(", ");
        }

        static QString PayloadFor(const QStringList &args) {
            const QString key = args.value(0).mid(3);
            return key + ", " + StoredValue(key, args);
        }

        void SendListing(const QString &command, const QString &body) {
            // p.62: "$R;" header, the "---->" pseudo-prompt, then formatted
            // blocks; only the last one ends with the real prompt.
            Send("$R; " + command + "\r\n---->\r\n");
            Send("$-- BLOCK 1 / 1\r\n" + body + "\r\n" + Prompt() + "\r\n");
        }

        QObject ctx_;
        QTcpServer server_;
        QTcpSocket *socket_{nullptr};
        QByteArray buffer_;
        QString cd_;
        QStringList commands_;
        QStringList config_lines_;
        QHash<QString, QString> store_;
    };

    // ------------------------------------------------------------- fixtures

    asterx::ReceiverSettings MakeReceiver() {
        asterx::ReceiverSettings s;
        s.user = "admin";
        s.password = "secret";
        s.antenna.lever_arm_m = asterx::Vec3{0.1, -0.2, 0.3};
        s.sbf_streams = {{1, {"Status"}, "OnChange"}};
        s.warmup.min_uptime_s = 0; // most tests are about the command sequence
        s.warmup.require_finetime = false;
        return s;
    }

    asterx::AppConfig MakeConfig(const std::filesystem::path &dir, quint16 port) {
        asterx::AppConfig c;
        c.host = "127.0.0.1";
        c.ctrl_port = port;
        c.output_dir = dir;
        c.file_prefix = "asterx";
        c.rotate_bytes = 1u << 20;
        c.rotate_interval_seconds = 3600;
        c.live_csv = false;
        c.stats_period_ms = 100000; // out of the way
        c.receiver = MakeReceiver();
        return c;
    }

    // Session plus the flags its two signals raise
    struct Harness {
        explicit Harness(asterx::AppConfig cfg) : session(std::move(cfg)) {
            QObject::connect(&session, &asterx::Session::Configured, &ctx, [this] { ++configured; });
            QObject::connect(&session, &asterx::Session::FatalError, &ctx, [this] { fatal = true; });
        }

        QObject ctx;
        asterx::Session session;
        int configured{0};
        bool fatal{false};
    };
} // namespace


TEST_CASE("Session: ConfiguresTheReceiverAndStartsRecording") {
    const auto dir = Scratch("ok");
    FakeReceiver rx;
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.configured > 0 || h.fatal; }));
    CHECK_FALSE(h.fatal);
    CHECK(h.configured == 1);

    // The sequence the driver documents, in order and without the retired steps
    const QString login = "login, admin, secret, RxAdmin, S3pt3ntr10";
    REQUIRE(rx.Commands().size() >= 4);
    CHECK(rx.Commands().at(0) == login);
    CHECK(rx.Commands().at(1) == "exeCopyConfigFile, RxDefault, Current");
    CHECK(rx.Commands().at(2) == login);
    CHECK(rx.Commands().at(3) == "getReceiverCapabilities");
    CHECK_FALSE(rx.Saw("lstCurrentUser"));
    CHECK_FALSE(rx.Saw("lstAntennaInfo, Overview")); // both antenna types are Unknown

    // The reset precedes every read-only step, and the listing precedes the
    // SBF streams that would otherwise interleave with it.
    CHECK(rx.IndexOf("exeCopyConfigFile, RxDefault, Current") < rx.IndexOf("getReceiverCapabilities"));
    CHECK(rx.IndexOf("lstConfigFile, Current") < rx.IndexOf("setSBFOutput, Stream1, IP10, Status, OnChange"));
    // Time-critical group first after the reset
    CHECK(rx.IndexOf("setPPSParameters, sec1, Low2High, 0.00, GPS, 60, 5.000000") < rx.IndexOf("setDataInOut, IP10, , +SBF"));
    CHECK(rx.Saw("setNTPServer, off"));
    CHECK(rx.Saw("setINSNavConfig, on, all, POI1"));
    CHECK(rx.Saw("setSBFOutput, Stream1, IP10, Status, OnChange"));

    h.session.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: RejectedLoginIsAStartupFailure") {
    const auto dir = Scratch("login");
    FakeReceiver rx;
    rx.reject_command = "login";
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.fatal; }));
    CHECK(h.configured == 0);
    // No reconnect loop before the first successful configure
    CHECK(rx.Commands().size() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: MissingAux1IsAStartupFailure") {
    const auto dir = Scratch("aux");
    FakeReceiver rx;
    rx.capabilities_antennas = "Main";
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.fatal; }));
    CHECK(h.configured == 0);
    CHECK(rx.Saw("getReceiverCapabilities"));
    CHECK_FALSE(rx.Saw("setPPSParameters, sec1, Low2High, 0.00, GPS, 60, 5.000000"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: ForeignLineInTheRunningConfigurationIsAStartupFailure") {
    const auto dir = Scratch("foreign");
    FakeReceiver rx;
    rx.extra_config_lines << "setSmoothingInterval, all, 100";
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.fatal; }));
    CHECK(h.configured == 0);
    CHECK(rx.Saw("lstConfigFile, Current"));
    CHECK_FALSE(rx.Saw("setSBFOutput, Stream1, IP10, Status, OnChange"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: StrayPromptBeforeAListingDoesNotShiftTheSequence") {
    // SsnRx writes "\r\n" on its own after 5 s without an ASCII reply, and the
    // receiver answers it with a bare prompt. Completing the listing on that
    // prompt used to consume the real "$R;" reply as the NEXT command's reply
    // and shift every later verification by one.
    const auto dir = Scratch("stray");
    FakeReceiver rx;
    rx.stray_prompt_before = "lstConfigFile";
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.configured > 0 || h.fatal; }));
    CHECK_FALSE(h.fatal);
    CHECK(h.configured == 1);
    CHECK(rx.Saw("setSBFOutput, Stream1, IP10, Status, OnChange"));

    h.session.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: ReplyThatEchoesAnotherCommandIsIgnored") {
    const auto dir = Scratch("echo");
    FakeReceiver rx;
    rx.wrong_echo_before = "getPPSParameters";
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.configured > 0 || h.fatal; }));
    CHECK_FALSE_MESSAGE(h.fatal, "the mis-addressed reply must be dropped, not acted on");
    CHECK(h.configured == 1);
    CHECK(rx.Saw("setSBFOutput, Stream1, IP10, Status, OnChange"));

    h.session.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: SbfArrivingBeforeTheResetIsIgnored") {
    // The previous session's setSBFOutput lines are still armed on this IPxx
    // until the reset runs, so blocks interleave with the first replies.
    const auto dir = Scratch("early_sbf");
    FakeReceiver rx;
    const auto frame = ReceiverStatusFrame(500, 0);
    rx.after_command = [&rx, frame](const QString &cmd) {
        if (cmd.startsWith("login")) {
            rx.SendSbf(frame);
            rx.SendSbf(frame);
        }
    };
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.configured > 0 || h.fatal; }));
    CHECK_FALSE(h.fatal);
    CHECK(h.configured == 1);

    h.session.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: WarmUpGateHoldsRecordingUntilUptimeAndFinetime") {
    const auto dir = Scratch("warmup");
    FakeReceiver rx;
    auto cfg = MakeConfig(dir, rx.Port());
    cfg.receiver.warmup.min_uptime_s = 1200;
    cfg.receiver.warmup.require_finetime = true;
    Harness h(std::move(cfg));
    h.session.Start();

    // The whole sequence runs, but recording waits for ReceiverStatus
    REQUIRE(SpinUntil([&] { return rx.Saw("setSBFOutput, Stream1, IP10, Status, OnChange"); }));
    Pump(200); // let any pending signal land
    CHECK(h.configured == 0);
    CHECK_FALSE(h.fatal);

    constexpr std::uint32_t kFineTime = 1u << 6;
    const auto prewarm_block = ReceiverStatusFrame(600, kFineTime);
    rx.SendSbf(prewarm_block); // too young; must still survive in native SBF context
    Pump(200);
    CHECK(h.configured == 0);

    rx.SendSbf(ReceiverStatusFrame(1300, 0)); // old enough, but no FINETIME
    Pump(200);
    CHECK(h.configured == 0);

    rx.SendSbf(ReceiverStatusFrame(1300, kFineTime));
    REQUIRE(SpinUntil([&] { return h.configured > 0; }));
    CHECK_FALSE(h.fatal);

    h.session.Shutdown();
    bool preserved = false;
    for (const auto &entry : std::filesystem::directory_iterator(dir / "prewarm")) {
        if (entry.path().extension() != ".sbf") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (data.size() >= prewarm_block.size() &&
            std::equal(prewarm_block.begin(), prewarm_block.end(), data.begin())) preserved = true;
    }
    CHECK(preserved);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: DiskFailureDuringRecordingIsFatal") {
    // The .sbf file is the record of truth: a write that cannot land must stop
    // the driver, not scroll past as an error line while the rig keeps going.
    const auto dir = Scratch("disk");
    FakeReceiver rx;
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();

    REQUIRE(SpinUntil([&] { return h.configured > 0 || h.fatal; }));
    REQUIRE_FALSE(h.fatal);

    std::filesystem::remove_all(dir); // the output directory disappears mid-run
    rx.SendSbf(ReceiverStatusFrame(1300, 1u << 6));

    CHECK_MESSAGE(SpinUntil([&] { return h.fatal; }), "a failed SBF write must raise fatalError, not just log");

    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: RecoverableCrcErrorStillMarksTheRecordingIncomplete") {
    const auto dir = Scratch("crc-integrity");
    FakeReceiver rx;
    Harness h(MakeConfig(dir, rx.Port()));
    h.session.Start();
    REQUIRE(SpinUntil([&] { return h.configured > 0 || h.fatal; }));
    REQUIRE_FALSE(h.fatal);
    auto damaged = ReceiverStatusFrame(1300, 1u << 6);
    damaged[2] ^= 0x01; // bad CRC, valid framing; the parser must recover on the next block
    rx.SendSbf(damaged);
    rx.SendSbf(ReceiverStatusFrame(1301, 1u << 6));
    REQUIRE(SpinUntil([&] { return h.session.RecordingIncomplete(); }));
    CHECK_FALSE(h.fatal); // recording can continue, but its loss state is sticky
    h.session.Shutdown();
    CHECK(h.session.FinalStatistics().at("crc_errors").get<std::uint64_t>() >= 1);
    CHECK(h.session.RecordingIncomplete());
    std::filesystem::remove_all(dir);
}

TEST_CASE("Session: VerifiesAntennaNamesAgainstTheReceiverList") {
    const auto dir = Scratch("antenna");
    FakeReceiver rx;
    auto cfg = MakeConfig(dir, rx.Port());
    cfg.receiver.antenna.main_type = "AERAT2775_159   SPKE";
    Harness ok(std::move(cfg));
    ok.session.Start();
    REQUIRE(SpinUntil([&] { return ok.configured > 0 || ok.fatal; }));
    CHECK_FALSE(ok.fatal);
    CHECK(rx.Saw("lstAntennaInfo, Overview"));
    CHECK(rx.Saw("setAntennaType, Main, \"AERAT2775_159   SPKE\""));
    ok.session.Shutdown();

    // A name the receiver does not know is a startup failure, not a silent
    // fall-back to "no phase-centre model" (p.107).
    FakeReceiver rx2;
    auto bad_cfg = MakeConfig(dir, rx2.Port());
    bad_cfg.receiver.antenna.main_type = "NOT_A_REAL_ANT";
    Harness bad(std::move(bad_cfg));
    bad.session.Start();
    REQUIRE(SpinUntil([&] { return bad.fatal; }));
    CHECK(bad.configured == 0);
    CHECK_FALSE(rx2.Saw("setAntennaType, Main, NOT_A_REAL_ANT"));

    std::filesystem::remove_all(dir);
}
