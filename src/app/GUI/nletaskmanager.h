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

#ifndef NLETASKMANAGER_H
#define NLETASKMANAGER_H

#include <QObject>
#include <QSharedPointer>

#include <atomic>

// kdenlive TaskManager 式后台任务抽象：run() 在 worker 线程执行，
// 取消是协作式的（worker 轮询 mCanceled），进度经队列信号回 GUI
class NleTask : public QObject {
    Q_OBJECT
public:
    enum class State { Queued, Running, Done, Failed, Canceled };

    explicit NleTask(const QString& title, QObject* const parent = nullptr)
        : QObject(parent), mTitle(title) {}

    State state() const { return mState; }
    const QString& title() const { return mTitle; }
    const QString& detail() const { return mDetail; }
    // 0..1；<0 = 不确定进度
    qreal progress() const { return mProgress; }
    bool cancelable() const { return mCancelable; }
    bool canceled() const { return mCanceled.load(); }

    void requestCancel() {
        mCanceled.store(true);
        cancelHook();
    }
    // 子类在 run() 内部设置；跨线程经队列信号落 GUI
    void setDetail(const QString& d);
    void setProgress(const qreal p);
    void setCancelable(const bool c) { mCancelable = c; }
signals:
    void progressChanged();
    void stateChanged();
    void finished(const int state, const QString& err);
protected:
    // worker 线程主体；返回空 = 成功，非空 = 错误信息
    virtual QString run() = 0;
    // 取消钩子（GUI 线程调用）：杀进程等即时动作
    virtual void cancelHook() {}
private:
    friend class NleTaskManager;
    friend class TaskRunnable;
    void execute();  // manager 在 worker 线程调用

    QString mTitle;
    QString mDetail;
    qreal mProgress = -1.;
    State mState = State::Queued;
    bool mCancelable = true;
    std::atomic_bool mCanceled{false};
};

using NleTaskPtr = QSharedPointer<NleTask>;

class QThreadPool;
class TaskRunnable;

// 全局任务队列：专用线程池执行 + 并发上限（转码类任务吃满核会
// 卡渲染线程，限半数核封顶4），面板经 tasks() 只读列表渲染
class NleTaskManager : public QObject {
    Q_OBJECT
public:
    static NleTaskManager* instance();

    void submit(const NleTaskPtr& task);
    QList<NleTaskPtr> tasks() const { return mTasks; }
    int runningCount() const;
    void clearFinished();
signals:
    void tasksChanged();
private:
    explicit NleTaskManager();
    static NleTaskManager* sInstance;
    friend class TaskRunnable;
    void afterTaskFinished();

    QThreadPool* mPool = nullptr;
    QList<NleTaskPtr> mTasks;
    int mMaxConcurrent = 2;
};

#endif // NLETASKMANAGER_H
