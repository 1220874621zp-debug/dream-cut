/*
#
# Dream Cut - based on Friction
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "subtitlepanel.h"

#include <QFileDialog>
#include <QComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <utility>
#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <QHBoxLayout>
#include <QListWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "mainwindow.h"
#include "timelinedockwidget.h"
#include "Timeline/nletimelinemodel.h"

SubtitlePanel::SubtitlePanel(MainWindow* const mainWin)
    : QWidget(mainWin), mMainWindow(mainWin) {
    auto* const importBtn = new QToolButton(this);
    importBtn->setText(tr("导入SRT"));
    importBtn->setToolTip(tr("导入 SRT 字幕：每条建一个底部居中的文字块"));
    auto* const exportBtn = new QToolButton(this);
    exportBtn->setText(tr("导出SRT"));
    exportBtn->setToolTip(tr("把字幕轨的文字块导出为 SRT"));
    auto* const refreshBtn = new QToolButton(this);
    refreshBtn->setText(tr("刷新"));
    auto* const speechBtn = new QToolButton(this);
    speechBtn->setText(tr("语音识别"));
    speechBtn->setToolTip(
                tr("语音转字幕：whisper.cpp 识别后自动导入字幕轨"));

    auto* const bar = new QHBoxLayout();
    bar->setContentsMargins(4, 4, 4, 2);
    bar->setSpacing(4);
    bar->addWidget(importBtn);
    bar->addWidget(exportBtn);
    bar->addWidget(speechBtn);
    bar->addStretch(1);
    bar->addWidget(refreshBtn);

    mList = new QListWidget(this);
    mList->setWordWrap(true);

    auto* const lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    lay->addLayout(bar);
    lay->addWidget(mList, 1);

    const auto model = [this]() -> NleTimelineModel* {
        const auto tl = mMainWindow->getTimeLineWidget();
        return tl ? tl->nleModel() : nullptr;
    };

    connect(importBtn, &QToolButton::clicked, this, [this, model]() {
        const QString path = QFileDialog::getOpenFileName(
                    this, tr("导入字幕"),
                    QString(),
                    tr("字幕文件 (*.srt);;所有文件 (*)"));
        if (path.isEmpty()) { return; }
        const auto m = model();
        if (!m) { emit logMessage(tr("时间轴不可用")); return; }
        if (m->requestSubtitleImport(path)) { refresh(); }
    });
    connect(exportBtn, &QToolButton::clicked, this, [this, model]() {
        const QString path = QFileDialog::getSaveFileName(
                    this, tr("导出字幕"),
                    QStringLiteral("subtitles.srt"),
                    tr("字幕文件 (*.srt)"));
        if (path.isEmpty()) { return; }
        const auto m = model();
        if (!m) { emit logMessage(tr("时间轴不可用")); return; }
        m->requestSubtitleExport(path);
    });
    connect(refreshBtn, &QToolButton::clicked,
            this, &SubtitlePanel::refresh);
    connect(speechBtn, &QToolButton::clicked,
            this, &SubtitlePanel::openSpeechDialog);
    // 无头台架：DREAMCUT_STT_AUTOTEST=1 ——管线缺件降级 + ffmpeg
    // wav 抽取真跑（[AUTOTESTSTT] 落日志）
    if (qEnvironmentVariableIsSet("DREAMCUT_STT_AUTOTEST")) {
        QTimer::singleShot(2500, this, [this]() {
            const QString bin = SpeechToText::findWhisperBinary();
            qInfo("[AUTOTESTSTT] whisperBin=%s",
                  qUtf8Printable(bin.isEmpty()
                                 ? QStringLiteral("(none)") : bin));
            // ffmpeg 真跑：44.1k 立体声 → 16k 单声道
            const QString src = QStringLiteral(
                        "/tmp/dcstt_%1_in.wav").arg(
                        QCoreApplication::applicationPid());
            QProcess::execute(QStringLiteral("python3"),
                {QStringLiteral("-c"),
                 QStringLiteral(
                     "import wave,struct,math;"
                     "w=wave.open('%1','w');w.setnchannels(2);"
                     "w.setsampwidth(2);w.setframerate(44100);"
                     "f=[]\n"
                     "for i in range(44100):"
                     "v=struct.pack('<hh',int(9000*math.sin("
                     "2*math.pi*440*i/44100)),0);f.append(v)\n"
                     "w.writeframes(b''.join(f));w.close()").arg(src)});
            const QString dst = QStringLiteral(
                        "/tmp/dcstt_%1_out.wav").arg(
                        QCoreApplication::applicationPid());
            QString err;
            const bool ok = SpeechToText::extractWav(src, dst, &err);
            QString meta;
            {
                QFile f(dst);
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray h = f.read(44);
                    if (h.size() >= 44) {
                        const int rate = h.at(24) & 0xff
                                | (h.at(25) & 0xff) << 8
                                | (h.at(26) & 0xff) << 16
                                | (h.at(27) & 0xff) << 24;
                        const int ch = h.at(22) & 0xff
                                | (h.at(23) & 0xff) << 8;
                        meta = QStringLiteral("rate=%1 ch=%2")
                                .arg(rate).arg(ch);
                    }
                }
            }
            QFile::remove(src);
            QFile::remove(dst);
            qInfo("[AUTOTESTSTT] extractWav=%d %s errLen=%d",
                  int(ok), qUtf8Printable(meta),
                  int(err.length()));
            qInfo("[AUTOTESTSTT] DONE");
        });
    }

    connect(mList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* const item) {
        if (!item) { return; }
        const int frame = item->data(Qt::UserRole).toInt();
        const auto tl = mMainWindow->getTimeLineWidget();
        if (tl) { tl->nleSeek(frame); }
    });
}

void SubtitlePanel::refresh()
{
    mList->clear();
    const auto tl = mMainWindow->getTimeLineWidget();
    const auto model = tl ? tl->nleModel() : nullptr;
    if (!model) { return; }
    const int tid = model->subtitleTrackId();
    if (tid < 0) {
        auto* const item = new QListWidgetItem(tr("没有字幕轨（导入SRT自动创建）"));
        item->setFlags(Qt::NoItemFlags);
        mList->addItem(item);
        return;
    }
    const qreal fps = model->fps() > 0 ? model->fps() : 24.;
    const auto tc = [fps](const int frame) {
        const qreal secs = frame/fps;
        const int m = int(secs)/60;
        const qreal s = secs - m*60;
        return QStringLiteral("%1:%2")
                .arg(m, 2, 10, QLatin1Char('0'))
                .arg(s, 4, 'f', 1, QLatin1Char('0'));
    };
    // 收集字幕轨的 TextBox（按起点排序）
    struct Item { int st; int en; QString text; };
    QList<Item> items;
    for (const auto &c : model->clips()) {
        if (c.trackId != tid || !c.layer) { continue; }
        items << Item{c.start, c.start + c.duration, c.name};
    }
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.st < b.st; });
    for (const auto &it : items) {
        auto* const item = new QListWidgetItem(
                    QStringLiteral("[%1→%2] %3")
                    .arg(tc(it.st), tc(it.en), it.text));
        item->setData(Qt::UserRole, it.st);
        item->setToolTip(it.text);
        mList->addItem(item);
    }
}


// ---- 语音转字幕（whisper.cpp 外挂编排）----

QString SpeechToText::findWhisperBinary()
{
    static const char* names[] = {"whisper-cli", "whisper-cli-main",
                                  "whisper-cpp", "main"};
    for (const auto* n : names) {
        const QString p = QStandardPaths::findExecutable(
                    QString::fromLatin1(n));
        if (!p.isEmpty()) { return p; }
    }
    return QString();
}

bool SpeechToText::extractWav(const QString& media, const QString& wavOut,
                              QString* errOut)
{
    QProcess p;
    p.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-y"), QStringLiteral("-i"), media,
             QStringLiteral("-ar"), QStringLiteral("16000"),
             QStringLiteral("-ac"), QStringLiteral("1"),
             QStringLiteral("-vn"), wavOut});
    p.waitForFinished(120000);
    const bool ok = p.exitCode() == 0 && QFileInfo::exists(wavOut);
    if (!ok && errOut) {
        *errOut = QString::fromUtf8(p.readAllStandardError())
                .right(400);
    }
    return ok;
}

void SubtitlePanel::openSpeechDialog()
{
    const auto tl = mMainWindow->getTimeLineWidget();
    const auto model = tl ? tl->nleModel() : nullptr;
    if (!model) {
        emit logMessage(tr("时间轴不可用"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("语音转字幕"));
    const auto grid = new QGridLayout(&dlg);

    const auto row = [&grid, &dlg](const QString& label,
                             const QString& hint,
                             const QString& val) {
        const int r = grid->rowCount();
        grid->addWidget(new QLabel(label, &dlg), r, 0);
        const auto edit = new QLineEdit(val, &dlg);
        edit->setPlaceholderText(hint);
        grid->addWidget(edit, r, 1);
        const auto browse = new QPushButton(tr("浏览…"), &dlg);
        grid->addWidget(browse, r, 2);
        return std::pair<QLineEdit*, QPushButton*>(edit, browse);
    };

    QSettings s;
    QString binHint = SpeechToText::findWhisperBinary();
    const auto binRow = row(tr("whisper 程序："),
                            tr("whisper-cli 可执行文件路径"),
                            s.value(QStringLiteral("speech/whisperBin"),
                                    binHint).toString());
    const auto modelRow = row(tr("识别模型："),
                              tr("ggml 模型 .bin（如 ggml-base.bin）"),
                              s.value(QStringLiteral("speech/model"))
                              .toString());
    const auto mediaRow = row(tr("音视频文件："),
                              tr("要识别的音频或视频"),
                              QString());

    grid->addWidget(new QLabel(tr("语言："), &dlg), 3, 0);
    const auto lang = new QComboBox(&dlg);
    lang->addItem(tr("自动检测"), QStringLiteral("auto"));
    lang->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    lang->addItem(QStringLiteral("English"), QStringLiteral("en"));
    lang->addItem(QStringLiteral("日本語"), QStringLiteral("ja"));
    grid->addWidget(lang, 3, 1, 1, 2);

    const auto log = new QPlainTextEdit(&dlg);
    log->setReadOnly(true);
    log->setPlaceholderText(tr("进度日志"));
    grid->addWidget(log, 4, 0, 1, 3);
    if (binHint.isEmpty()) {
        log->setPlainText(
                    tr("未在系统中找到 whisper-cli。请安装 whisper.cpp"
                       "（https://github.com/ggml-org/whisper.cpp）并"
                       "下载 ggml 模型，或直接在上方指定程序与模型"
                       "路径。"));
    }

    const auto btns = new QHBoxLayout();
    const auto startBtn = new QPushButton(tr("开始识别"), &dlg);
    const auto closeBtn = new QPushButton(tr("关闭"), &dlg);
    btns->addStretch(1);
    btns->addWidget(startBtn);
    btns->addWidget(closeBtn);
    grid->addLayout(btns, 5, 0, 1, 3);
    dlg.setMinimumSize(520, 340);

    connect(binRow.second, &QPushButton::clicked, &dlg, [e = binRow.first]() {
        const QString p = QFileDialog::getOpenFileName(
                    e, QObject::tr("选择 whisper 程序"));
        if (!p.isEmpty()) { e->setText(p); }
    });
    connect(modelRow.second, &QPushButton::clicked, &dlg,
            [e = modelRow.first]() {
        const QString p = QFileDialog::getOpenFileName(
                    e, QObject::tr("选择 ggml 模型"),
                    QString(), QObject::tr("模型 (*.bin)"));
        if (!p.isEmpty()) { e->setText(p); }
    });
    connect(mediaRow.second, &QPushButton::clicked, &dlg,
            [e = mediaRow.first]() {
        const QString p = QFileDialog::getOpenFileName(
                    e, QObject::tr("选择音视频"));
        if (!p.isEmpty()) { e->setText(p); }
    });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    // 管线状态机：ffmpeg 抽 wav → whisper 识别 → 导入 SRT
    enum class Stage { Idle, Wav, Whisper, Done };
    struct Pipe {
        Stage stage = Stage::Idle;
        QString wav;
        QString srt;
        QProcess* proc = nullptr;
    } pipe;

    connect(startBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString bin = binRow.first->text().trimmed();
        const QString mdl = modelRow.first->text().trimmed();
        const QString media = mediaRow.first->text().trimmed();
        if (bin.isEmpty() || mdl.isEmpty() || media.isEmpty()) {
            log->appendPlainText(tr("请先指定 whisper 程序、模型与"
                                    "音视频文件"));
            return;
        }
        if (!QFileInfo::exists(media)) {
            log->appendPlainText(tr("音视频文件不存在"));
            return;
        }
        startBtn->setEnabled(false);
        s.setValue(QStringLiteral("speech/whisperBin"), bin);
        s.setValue(QStringLiteral("speech/model"), mdl);
        // wav/srt 落临时目录（whisper --output-srt 写在 wav 旁）
        const QString base = QStringLiteral("dcstt_%1")
                .arg(QCoreApplication::applicationPid());
        pipe.wav = QStandardPaths::writableLocation(
                    QStandardPaths::TempLocation) + QLatin1Char('/')
                + base + QStringLiteral(".wav");
        pipe.srt = QStandardPaths::writableLocation(
                    QStandardPaths::TempLocation) + QLatin1Char('/')
                + base + QStringLiteral(".srt");
        pipe.stage = Stage::Wav;
        QString err;
        if (!SpeechToText::extractWav(media, pipe.wav, &err)) {
            log->appendPlainText(tr("音频提取失败：ffmpeg 缺失或文件"
                                    "无法解码"));
            if (!err.isEmpty()) { log->appendPlainText(err); }
            startBtn->setEnabled(true);
            pipe.stage = Stage::Idle;
            return;
        }
        log->appendPlainText(tr("音频已提取，开始识别…"));
        pipe.stage = Stage::Whisper;
        pipe.proc = new QProcess(&dlg);
        connect(pipe.proc, &QProcess::readyReadStandardError, &dlg,
                [pr = pipe.proc, lg = log]() {
            lg->appendPlainText(
                        QString::fromUtf8(pr->readAllStandardError())
                        .right(2000));
        });
        connect(pipe.proc,
                qOverload<int, QProcess::ExitStatus>(
                    &QProcess::finished),
                &dlg, [&](const int code, QProcess::ExitStatus) {
            if (pipe.stage != Stage::Whisper) { return; }
            pipe.stage = Stage::Done;
            if (code != 0 || !QFileInfo::exists(pipe.srt)) {
                log->appendPlainText(
                            tr("识别失败（退出码 %1）").arg(code));
                startBtn->setEnabled(true);
                return;
            }
            log->appendPlainText(tr("识别完成，导入字幕…"));
            if (model->requestSubtitleImport(pipe.srt)) {
                refresh();
                log->appendPlainText(tr("已导入字幕轨"));
                emit logMessage(tr("语音识别字幕已导入"));
            } else {
                log->appendPlainText(tr("字幕导入失败"));
            }
            QFile::remove(pipe.wav);
            QFile::remove(pipe.srt);
            startBtn->setEnabled(true);
        });
        // whisper.cpp 的 -of 是输出基底名（--output-srt 会补
        // .srt 扩展），传完整 .srt 路径会叠成 .srt.srt
        const QString outBase = pipe.srt.section(
                    QStringLiteral("."), 0, -2);
        pipe.proc->start(bin,
                         {QStringLiteral("-m"), mdl,
                          QStringLiteral("-f"), pipe.wav,
                          QStringLiteral("-l"),
                          lang->currentData().toString(),
                          QStringLiteral("--output-srt"),
                          QStringLiteral("-of"), outBase});
    });

    dlg.exec();
    if (pipe.proc) { pipe.proc->kill(); }
    if (pipe.stage == Stage::Whisper) {
        QFile::remove(pipe.wav);
        QFile::remove(pipe.srt);
    }
}
