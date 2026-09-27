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

#include "nletaskpanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "nletaskmanager.h"

NleTaskPanel::NleTaskPanel(QWidget* const parent) : QWidget(parent)
{
    mHost = new QWidget(this);
    mListLayout = new QVBoxLayout(mHost);
    mListLayout->setContentsMargins(4, 4, 4, 4);
    mListLayout->setSpacing(4);

    mEmptyLabel = new QLabel(tr("没有后台任务"), mHost);
    mEmptyLabel->setStyleSheet(QStringLiteral("color:#888;"));
    mListLayout->addWidget(mEmptyLabel);
    mListLayout->addStretch(1);

    auto* const scroll = new QScrollArea(this);
    scroll->setWidget(mHost);
    scroll->setWidgetResizable(true);

    auto* const clearBtn = new QToolButton(this);
    clearBtn->setText(tr("清除已完成"));

    auto* const bar = new QHBoxLayout();
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addStretch(1);
    bar->addWidget(clearBtn);

    auto* const lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(bar);
    lay->addWidget(scroll, 1);

    const auto mgr = NleTaskManager::instance();
    connect(mgr, &NleTaskManager::tasksChanged,
            this, &NleTaskPanel::rebuild);
    connect(clearBtn, &QToolButton::clicked,
            mgr, &NleTaskManager::clearFinished);
    rebuild();
}

void NleTaskPanel::rebuild()
{
    // 清旧行（保留 stretch 最后重加）
    while (mListLayout->count() > 0) {
        const auto item = mListLayout->takeAt(0);
        if (item->widget()) { item->widget()->deleteLater(); }
        delete item;
    }
    const auto& tasks = NleTaskManager::instance()->tasks();
    if (tasks.isEmpty()) {
        auto* const lbl = new QLabel(tr("没有后台任务"), mHost);
        lbl->setStyleSheet(QStringLiteral("color:#888;"));
        mListLayout->addWidget(lbl);
        mListLayout->addStretch(1);
        return;
    }
    for (const auto& t : tasks) {
        const auto row = new QWidget(mHost);
        const auto title = new QLabel(t->title(), row);
        title->setToolTip(t->detail());
        const auto bar = new QProgressBar(row);
        bar->setRange(0, 100);
        bar->setTextVisible(false);
        bar->setFixedHeight(8);
        const auto p = t->progress();
        if (p >= 0.) { bar->setValue(qRound(p*100)); }
        else { bar->setRange(0, 0); }  // 不定进度

        const auto stateTxt = [t]() {
            switch (t->state()) {
            case NleTask::State::Queued:
                return NleTaskPanel::tr("排队中");
            case NleTask::State::Running:
                return t->progress() >= 0.
                        ? QStringLiteral("%1%").arg(
                              qRound(t->progress()*100))
                        : NleTaskPanel::tr("进行中");
            case NleTask::State::Done:
                return NleTaskPanel::tr("完成");
            case NleTask::State::Failed:
                return NleTaskPanel::tr("失败");
            case NleTask::State::Canceled:
                return NleTaskPanel::tr("已取消");
            }
            return QString();
        }();
        const auto state = new QLabel(stateTxt, row);
        state->setMinimumWidth(52);

        const auto cancel = new QToolButton(row);
        cancel->setText(tr("取消"));
        cancel->setEnabled(t->cancelable() &&
                           (t->state() == NleTask::State::Queued ||
                            t->state() == NleTask::State::Running));
        connect(cancel, &QToolButton::clicked,
                t.data(), &NleTask::requestCancel);

        const auto col = new QVBoxLayout(row);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);
        const auto head = new QHBoxLayout();
        head->setSpacing(4);
        head->addWidget(title, 1);
        head->addWidget(state);
        head->addWidget(cancel);
        col->addLayout(head);
        col->addWidget(bar);

        mListLayout->addWidget(row);
    }
    mListLayout->addStretch(1);
}
