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

#include <algorithm>
#include <QHBoxLayout>
#include <QListWidget>
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

    auto* const bar = new QHBoxLayout();
    bar->setContentsMargins(4, 4, 4, 2);
    bar->setSpacing(4);
    bar->addWidget(importBtn);
    bar->addWidget(exportBtn);
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
