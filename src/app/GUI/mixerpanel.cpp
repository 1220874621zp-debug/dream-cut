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

#include "mixerpanel.h"

#include <cmath>

#include <QCoreApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "mainwindow.h"
#include "timelinedockwidget.h"
#include "Timeline/nletimelinemodel.h"
#include "Private/document.h"
#include "canvas.h"
#include "Sound/soundcomposition.h"

// ---- 电平条 ----

MixerMeter::MixerMeter(QWidget* const parent) : QWidget(parent) {
    setFixedSize(14, 170);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void MixerMeter::setLevel(const qreal rms) {
    mRms = rms;
    const qreal db = 20.*std::log10(qMax(rms, 1e-6));
    if(db > mPeakDb) { mPeakDb = db; }
    update();
}

void MixerMeter::tick() {
    // 峰值保持：每节拍缓慢回落
    if(mPeakDb > -99.) {
        mPeakDb -= 0.8;
        if(mPeakDb < -99.) { mPeakDb = -99.; }
        update();
    }
}

void MixerMeter::paintEvent(QPaintEvent* const e) {
    Q_UNUSED(e)
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1d, 0x1d, 0x1d));

    const qreal rmsDb = 20.*std::log10(qMax(mRms, 1e-6));
    const int h = height();
    const int rmsY = qRound(h*(1. - dbFrac(rmsDb)));
    for(int y = h - 1; y >= rmsY; y--) {
        const qreal db = qreal(h - 1 - y)/(h - 1)*66. - 60.;
        QColor c;
        if(db >= -3.) {
            c = QColor(0xe5, 0x3e, 0x3e);
        } else if(db >= -12.) {
            const qreal t = (db + 12.)/9.;
            c = QColor(qRound(0xd4 + t*(0xe5 - 0xd4)),
                       qRound(0xb1 + t*(0x3e - 0xb1)), 0x3e);
        } else {
            const qreal t = qBound(0., (db + 60.)/48., 1.);
            c = QColor(qRound(0x37 + t*(0xd4 - 0x37)), 0xb5, 0x50);
        }
        p.setPen(c);
        p.drawLine(0, y, width() - 1, y);
    }
    // 过载顶条
    if(mRms > 1.) { p.fillRect(0, 0, width(), 3, QColor(0xff, 0x2b, 0x2b)); }
    // 峰值保持线
    if(mPeakDb > -60.) {
        const int py = qRound(h*(1. - dbFrac(mPeakDb)));
        p.fillRect(0, qBound(0, py, h - 2), width(), 2, QColor(0xff, 0xff, 0xff));
    }
}

// ---- 单轨条 ----

MixerTrackStrip::MixerTrackStrip(QWidget* const parent) : QWidget(parent) {
    setFixedSize(78, 262);

    mNameLabel = new QLabel(this);
    mNameLabel->setAlignment(Qt::AlignHCenter);

    mMeter = new MixerMeter(this);

    mFader = new QSlider(Qt::Vertical, this);
    mFader->setRange(0, 66);            // 0 = -60dB（近静音），60 = 0dB
    mFader->setValue(60);
    mFader->setInvertedAppearance(true); // 大值在上
    mFader->setSingleStep(1);
    mFader->setPageStep(6);
    mFader->setToolTip(tr("轨道音量（-60..+6dB，双击空白归零）"));

    mDbLabel = new QLabel(QStringLiteral("0 dB"), this);
    mDbLabel->setAlignment(Qt::AlignHCenter);

    mMuteBtn = new QToolButton(this);
    mMuteBtn->setText(QStringLiteral("M"));
    mMuteBtn->setCheckable(true);
    mMuteBtn->setToolTip(tr("静音轨道"));
    mMuteBtn->setAutoRaise(true);

    mSoloBtn = new QToolButton(this);
    mSoloBtn->setText(QStringLiteral("S"));
    mSoloBtn->setCheckable(true);
    mSoloBtn->setToolTip(tr("独奏轨道"));
    mSoloBtn->setAutoRaise(true);

    auto* const mid = new QHBoxLayout();
    mid->setContentsMargins(6, 0, 6, 0);
    mid->setSpacing(4);
    mid->addWidget(mMeter);
    mid->addWidget(mFader);

    auto* const btns = new QHBoxLayout();
    btns->setSpacing(4);
    btns->addWidget(mMuteBtn);
    btns->addWidget(mSoloBtn);

    auto* const lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 4, 2, 4);
    lay->setSpacing(2);
    lay->addWidget(mNameLabel);
    lay->addLayout(mid);
    lay->addWidget(mDbLabel);
    lay->addLayout(btns);

    connect(mFader, &QSlider::valueChanged, this,
            [this](const int v) {
        const qreal db = v - 60.;
        mDbLabel->setText(v == 0 ? QStringLiteral("-inf")
                                 : QStringLiteral("%1 dB").arg(
                                       qRound(db)));
        emit volumeChanged(mTrackId, std::pow(10., db/20.));
    });
    connect(mMuteBtn, &QToolButton::clicked,
            this, [this]() { emit muteClicked(mTrackId); });
    connect(mSoloBtn, &QToolButton::clicked,
            this, [this]() { emit soloClicked(mTrackId); });
}

void MixerTrackStrip::setInfo(const int trackId, const QString& name) {
    mTrackId = trackId;
    mNameLabel->setText(name);
    mNameLabel->setToolTip(name);
}

void MixerTrackStrip::setVolumeDb(const qreal db) {
    const int v = qBound(0, qRound(db + 60.), 66);
    if(mFader->value() == v) { return; }
    const QSignalBlocker b(mFader);
    mFader->setValue(v);
    mDbLabel->setText(v == 0 ? QStringLiteral("-inf")
                             : QStringLiteral("%1 dB").arg(qRound(db)));
}

void MixerTrackStrip::setMs(const bool muted, const bool solo) {
    if(mMuteBtn->isChecked() != muted) {
        const QSignalBlocker b(mMuteBtn);
        mMuteBtn->setChecked(muted);
    }
    mMuteBtn->setStyleSheet(muted
            ? QStringLiteral(
                  "QToolButton:checked{background:#c0392b;color:white;}")
            : QString());
    if(mSoloBtn->isChecked() != solo) {
        const QSignalBlocker b(mSoloBtn);
        mSoloBtn->setChecked(solo);
    }
    mSoloBtn->setStyleSheet(solo
            ? QStringLiteral(
                  "QToolButton:checked{background:#e0a11b;color:white;}")
            : QString());
}

void MixerTrackStrip::mouseDoubleClickEvent(QMouseEvent* const e) {
    // 双击空白/标签区推子归零；QSlider 自消费的点击到不了这里
    if(e->button() == Qt::LeftButton && mTrackId >= 0) {
        mFader->setValue(60);
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

// ---- 面板 ----

MixerPanel::MixerPanel(MainWindow* const mainWin)
    : QWidget(mainWin), mMainWindow(mainWin) {
    mStripsHost = new QWidget(this);
    mStripsLayout = new QHBoxLayout(mStripsHost);
    mStripsLayout->setContentsMargins(4, 4, 4, 4);
    mStripsLayout->setSpacing(2);
    mStripsLayout->addWidget(mEmptyLabel = new QLabel(
                                 tr("没有音频轨"), mStripsHost));
    mEmptyLabel->setStyleSheet(QStringLiteral("color:#888;"));

    auto* const scroll = new QScrollArea(this);
    scroll->setWidget(mStripsHost);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* const lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(scroll);

    mPollTimer = new QTimer(this);
    connect(mPollTimer, &QTimer::timeout, this, &MixerPanel::pollTick);
    mPollTimer->start(33);

    // 无头台架：DREAMCUT_MIXER_AUTOTEST=1（见 mixerAutotestStage1）
    if (qEnvironmentVariableIsSet("DREAMCUT_MIXER_AUTOTEST")) {
        QTimer::singleShot(2500, this, &MixerPanel::mixerAutotestStage1);
    }
}

void MixerPanel::pollTick() {
    const auto tl = mMainWindow->getTimeLineWidget();
    const auto model = tl ? tl->nleModel() : nullptr;
    if(!model) { return; }
    const auto scene = model->panelScene();
    if(!scene) { return; }

    // 签名只含结构/状态（不含推子值——拖动中重建会拆掉手里的
    // 滑杆），推子由每拍回读同步（值相同即 no-op）
    QString sig;
    for(const auto& t : model->tracks()) {
        if(!t.audio) { continue; }
        sig += QStringLiteral("%1:%2:%3:%4 ")
                .arg(QString::number(t.id), t.name)
                .arg(t.muted ? 1 : 0).arg(t.solo ? 1 : 0);
    }
    sig.prepend(QStringLiteral("%1|").arg(
                    reinterpret_cast<qlonglong>(scene), 0, 16));
    if(sig != mSignature) {
        mSignature = sig;
        rebuildStrips();
    }

    const auto comp = scene->getSoundComposition();
    for(auto* const strip : mStripsHost->findChildren<MixerTrackStrip*>()) {
        const auto t = model->track(strip->trackId());
        if(!t) { continue; }
        strip->setMs(t->muted, t->solo);
        strip->setVolumeDb(20.*std::log10(
                               qMax(scene->trackSpecVolume(strip->trackId()),
                                    0.000001)));
        strip->meter()->setLevel(comp ? comp->trackLevelAt(strip->trackId())
                                      : 0.);
        strip->meter()->tick();
    }
}

void MixerPanel::rebuildStrips() {
    while(mStripsLayout->count() > 0) {
        const auto item = mStripsLayout->takeAt(0);
        if(item->widget()) { item->widget()->deleteLater(); }
        delete item;
    }
    const auto tl = mMainWindow->getTimeLineWidget();
    const auto model = tl ? tl->nleModel() : nullptr;
    if(!model) { return; }
    const auto scene = model->panelScene();
    if(!scene) { return; }

    const auto& tracks = model->tracks();
    const int audioCount = static_cast<int>(
                std::count_if(tracks.cbegin(), tracks.cend(),
                              [](const NleTimelineModel::Track& t) {
                                  return t.audio;
                              }));
    if(audioCount == 0) {
        auto* const lbl = new QLabel(tr("没有音频轨"), mStripsHost);
        lbl->setStyleSheet(QStringLiteral("color:#888;"));
        mStripsLayout->addWidget(lbl);
        return;
    }
    for(const auto& t : tracks) {
        if(!t.audio) { continue; }
        auto* const strip = new MixerTrackStrip(mStripsHost);
        strip->setInfo(t.id, t.name);
        strip->setVolumeDb(20.*std::log10(
                               qMax(scene->trackSpecVolume(t.id), 0.000001)));
        strip->setMs(t.muted, t.solo);
        connect(strip, &MixerTrackStrip::volumeChanged,
                this, [this, scene](const int trackId, const qreal vol) {
            scene->setTrackSpecVolume(trackId, vol);
            emit logMessage(tr("轨道音量 %1 dB").arg(
                                qRound(20.*std::log10(qMax(vol, 0.000001)))));
        });
        connect(strip, &MixerTrackStrip::muteClicked,
                this, [this](const int trackId) {
            const auto tlw = mMainWindow->getTimeLineWidget();
            if(tlw && tlw->nleModel()) {
                tlw->nleModel()->requestTrackToggleMute(trackId, false);
            }
        });
        connect(strip, &MixerTrackStrip::soloClicked,
                this, [this](const int trackId) {
            const auto tlw = mMainWindow->getTimeLineWidget();
            if(tlw && tlw->nleModel()) {
                tlw->nleModel()->requestTrackToggleSolo(trackId);
            }
        });
        mStripsLayout->addWidget(strip);
    }
    mStripsLayout->addStretch();
}

// 无头台架：建音频轨→推子双向链路→外部写同步→插音频块→M/S→
// 电平查询（结果 [AUTOTESTMX] 落 /tmp/friction_debug.log）
void MixerPanel::mixerAutotestStage1()
{
        const auto tl0 = mMainWindow->getTimeLineWidget();
    const auto model = tl0 ? tl0->nleModel() : nullptr;
    if (!model || !model->panelScene()) {
        // 零场景启动（无头台架常态）：建场景后重入一次
        if (mAutotestRetried) {
            qInfo("[AUTOTESTMX] ABORT no model/scene after retry");
            return;
        }
        mAutotestRetried = true;
        const auto doc = Document::sInstance;
        if (!doc) {
            qInfo("[AUTOTESTMX] ABORT no document");
            return;
        }
        const auto scene = doc->createNewScene();
        qInfo("[AUTOTESTMX] created scene=%d", scene != nullptr);
        QTimer::singleShot(600, this, &MixerPanel::mixerAutotestStage1);
        return;
    }
        const int tid = model->requestTrackAdd(true);
        qInfo("[AUTOTESTMX] add track id=%d", tid);
        QTimer::singleShot(150, this, [this, tid]() {
            const auto tl = mMainWindow->getTimeLineWidget();
            const auto model = tl ? tl->nleModel() : nullptr;
            if (!model || !model->panelScene()) { return; }
            auto scene = model->panelScene();
            MixerTrackStrip* strip = nullptr;
            for (auto* s :
                 mStripsHost->findChildren<MixerTrackStrip*>()) {
                if (s->trackId() == tid) { strip = s; break; }
            }
            qInfo("[AUTOTESTMX] strip built=%d faderDb=%.1f",
                  strip != nullptr,
                  strip ? strip->faderDb() : -999.);
            if (!strip) { return; }
            // 拖动链：滑杆 -6dB → spec 应为 10^(-6/20)
            strip->fader()->setValue(54);
            const qreal specVol = scene->trackSpecVolume(tid);
            qInfo("[AUTOTESTMX] fader->spec vol=%.4f",
                  double(specVol));
            QTimer::singleShot(150, this, [this, tid]() {
                const auto tl = mMainWindow->getTimeLineWidget();
                const auto model = tl ? tl->nleModel() : nullptr;
                if (!model || !model->panelScene()) { return; }
                auto scene = model->panelScene();
                MixerTrackStrip* strip = nullptr;
                for (auto* s :
                     mStripsHost->findChildren<MixerTrackStrip*>()) {
                    if (s->trackId() == tid) { strip = s; break; }
                }
                if (!strip) { return; }
                // 外部写推子 → 下一拍条目同步回 0dB
                scene->setTrackSpecVolume(tid, 1.);
                QTimer::singleShot(120, this, [this, tid]() {
                    const auto tl = mMainWindow->getTimeLineWidget();
                    const auto model =
                            tl ? tl->nleModel() : nullptr;
                    if (!model || !model->panelScene()) { return; }
                    MixerTrackStrip* strip = nullptr;
                    for (auto* s :
                         mStripsHost->findChildren<
                             MixerTrackStrip*>()) {
                        if (s->trackId() == tid) {
                            strip = s; break;
                        }
                    }
                    qInfo("[AUTOTESTMX] spec->fader db=%.1f",
                          strip ? double(strip->faderDb()) : -999.);
                    // 静音/独奏/电平：需要轨上有块，插一段正弦
                    const QString wav = QStringLiteral(
                                "/tmp/dcmix_%1.wav").arg(
                                QCoreApplication::applicationPid());
                    QProcess::execute(QStringLiteral("python3"),
                        {QStringLiteral("-c"),
                         QStringLiteral(
                             "import wave,struct,math;"
                             "w=wave.open('%1','w');"
                             "w.setnchannels(1);w.setsampwidth(2);"
                             "w.setframerate(44100);"
                             "f=[]\n"
                             "for i in range(44100*2):"
                             "f.append(struct.pack('<h',int(12000*"
                             "math.sin(2*math.pi*440*i/44100))))\n"
                             "w.writeframes(b''.join(f));w.close()"
                             ).arg(wav)});
                    model->requestInsertSound(wav, tid, 0);
                    QTimer::singleShot(1500, this,
                                       [this, tid, wav]() {
                        QFile::remove(wav);
                        const auto tl =
                                mMainWindow->getTimeLineWidget();
                        const auto model =
                                tl ? tl->nleModel() : nullptr;
                        if (!model || !model->panelScene()) {
                            return;
                        }
                        auto scene = model->panelScene();
                        MixerTrackStrip* strip = nullptr;
                        for (auto* s :
                             mStripsHost->findChildren<
                                 MixerTrackStrip*>()) {
                            if (s->trackId() == tid) {
                                strip = s; break;
                            }
                        }
                        if (!strip) {
                            qInfo("[AUTOTESTMX] strip gone");
                            return;
                        }
                        const auto tk = model->track(tid);
                        // M：静音（模型镜像应翻转）
                        strip->muteButton()->click();
                        QTimer::singleShot(150, this,
                            [this, tid]() {
                            const auto tl =
                                    mMainWindow->getTimeLineWidget();
                            const auto model = tl
                                    ? tl->nleModel() : nullptr;
                            if (!model || !model->panelScene()) {
                                return;
                            }
                            auto scene = model->panelScene();
                            MixerTrackStrip* strip = nullptr;
                            for (auto* s :
                                 mStripsHost->findChildren<
                                     MixerTrackStrip*>()) {
                                if (s->trackId() == tid) {
                                    strip = s; break;
                                }
                            }
                            const auto tk = model->track(tid);
                            qInfo("[AUTOTESTMX] mute clicked"
                                  " muted=%d",
                                  tk && tk->muted ? 1 : 0);
                            if (strip) { strip->muteButton()->click(); }
                            QTimer::singleShot(150, this,
                                [this, tid]() {
                                const auto tl = mMainWindow
                                        ->getTimeLineWidget();
                                const auto model = tl
                                        ? tl->nleModel()
                                        : nullptr;
                                if (!model
                                        || !model->panelScene()) {
                                    return;
                                }
                                auto scene =
                                        model->panelScene();
                                MixerTrackStrip* strip = nullptr;
                                for (auto* s :
                                     mStripsHost->findChildren<
                                         MixerTrackStrip*>()) {
                                    if (s->trackId() == tid) {
                                        strip = s;
                                        break;
                                    }
                                }
                                if (strip) {
                                    strip->soloButton()->click();
                                }
                                QTimer::singleShot(150, this,
                                    [this, tid]() {
                                    const auto tl = mMainWindow
                                            ->getTimeLineWidget();
                                    const auto model = tl
                                            ? tl->nleModel()
                                            : nullptr;
                                    if (!model
                                            || !model->panelScene()) {
                                        return;
                                    }
                                    auto scene =
                                            model->panelScene();
                                    MixerTrackStrip* strip =
                                            nullptr;
                                    for (auto* s :
                                         mStripsHost
                                         ->findChildren<
                                             MixerTrackStrip*>()) {
                                        if (s->trackId() == tid) {
                                            strip = s;
                                            break;
                                        }
                                    }
                                    const auto tk =
                                            model->track(tid);
                                    const qreal lvl = scene
                                            ->getSoundComposition()
                                            ? scene
                                              ->getSoundComposition()
                                              ->trackLevelAt(tid)
                                            : -1.;
                                    qInfo("[AUTOTESTMX] solo=%d"
                                          " unmuted=%d lvl=%.3f",
                                          tk && tk->solo ? 1 : 0,
                                          tk && !tk->muted
                                          ? 1 : 0,
                                          double(lvl));
                                    if (strip) {
                                        strip->soloButton()
                                                ->click();
                                    }
                                    qInfo("[AUTOTESTMX] DONE");
                                });
                            });
                        });
                    });
                });
            });
        });
}
