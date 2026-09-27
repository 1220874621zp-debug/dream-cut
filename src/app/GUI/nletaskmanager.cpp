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

#include "nletaskmanager.h"

#include <QRunnable>
#include <QThread>
#include <QThreadPool>

NleTaskManager* NleTaskManager::sInstance = nullptr;

void NleTask::setDetail(const QString& d)
{
    QMetaObject::invokeMethod(this, [this, d]() {
        mDetail = d;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void NleTask::setProgress(const qreal p)
{
    QMetaObject::invokeMethod(this, [this, p]() {
        mProgress = p;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

void NleTask::execute()
{
    if (mCanceled.load()) {
        QMetaObject::invokeMethod(this, [this]() {
            mState = State::Canceled;
            emit stateChanged();
            emit finished(int(State::Canceled), QString());
        }, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(this, [this]() {
        mState = State::Running;
        emit stateChanged();
    }, Qt::QueuedConnection);
    const QString err = run();
    const bool canceled = mCanceled.load();
    QMetaObject::invokeMethod(this, [this, err, canceled]() {
        if (canceled) {
            mState = State::Canceled;
            emit stateChanged();
            emit finished(int(State::Canceled), QString());
        } else if (err.isEmpty()) {
            mState = State::Done;
            emit stateChanged();
            emit finished(int(State::Done), QString());
        } else {
            mState = State::Failed;
            mDetail = err;
            emit stateChanged();
            emit finished(int(State::Failed), err);
        }
    }, Qt::QueuedConnection);
}

// 与头文件前置声明同名同作用域（friend 依赖此名）
class TaskRunnable : public QRunnable {
public:
    TaskRunnable(const NleTaskPtr& task,
                 NleTaskManager* const mgr)
        : mTask(task), mMgr(mgr) {}
    void run() override {
        mTask->execute();
        QMetaObject::invokeMethod(mMgr, [mgr = mMgr] {
            mgr->afterTaskFinished();
        }, Qt::QueuedConnection);
    }
private:
    NleTaskPtr mTask;
    NleTaskManager* mMgr;
};

NleTaskManager::NleTaskManager()
    : mPool(new QThreadPool(this))
{
    const int cores = QThread::idealThreadCount();
    // 专用池（不动全局池——渲染/解码任务在用）；转码吃满核会卡
    // 合成，限半数核、封顶 4
    mPool->setMaxThreadCount(qBound(1, cores/2, 4));
    mMaxConcurrent = mPool->maxThreadCount();
}

NleTaskManager* NleTaskManager::instance()
{
    if (!sInstance) { sInstance = new NleTaskManager(); }
    return sInstance;
}

int NleTaskManager::runningCount() const
{
    int n = 0;
    for (const auto& t : mTasks) {
        if (t->state() == NleTask::State::Running) { n++; }
    }
    return n;
}

void NleTaskManager::submit(const NleTaskPtr& task)
{
    mTasks.append(task);
    emit tasksChanged();
    connect(task.data(), &NleTask::stateChanged,
            this, &NleTaskManager::tasksChanged);
    connect(task.data(), &NleTask::progressChanged,
            this, &NleTaskManager::tasksChanged);
    mPool->start(new TaskRunnable(task, this));
}

void NleTaskManager::afterTaskFinished()
{
    emit tasksChanged();
}

void NleTaskManager::clearFinished()
{
    for (int i = mTasks.size() - 1; i >= 0; i--) {
        const auto s = mTasks.at(i)->state();
        if (s == NleTask::State::Done ||
                s == NleTask::State::Failed ||
                s == NleTask::State::Canceled) {
            mTasks.removeAt(i);
        }
    }
    emit tasksChanged();
}
