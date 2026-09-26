/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors
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

#ifndef INTERNALLINKCANVASBOX_H
#define INTERNALLINKCANVASBOX_H

#include "Boxes/internallinkbox.h"
#include "canvas.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QTimer>

// 外部工程场景的解析缓存：同一文件被多个链接盒引用时共享一份
// 内存场景；QFileSystemWatcher 监视源文件，外部编辑器保存后防抖
// 重解析并广播 projectReloaded，链接盒收到后重绑目标场景（AE→PR
// 动态链接的进程内实现——下游持活引用，源文件是唯一事实源）
class ExternalProjectCache : public QObject {
    Q_OBJECT
public:
    struct Loaded {
        QString path;
        QList<qsptr<Canvas>> scenes;
    };

    static ExternalProjectCache* instance();

    // forceReload=false 且缓存命中时直接复用；解析失败返回 nullptr
    // 并把原因写进 err
    QSharedPointer<Loaded> load(const QString& path,
                                const bool forceReload,
                                QString* err = nullptr);

    // 目标场景解析：优先 documentId（工程内稳定），失配退化按名
    // 匹配，再退化第一个场景
    Canvas* findScene(Loaded* loaded,
                      const int sceneDocId,
                      const QString& sceneName) const;

    static QStringList sceneNames(Loaded* loaded);
signals:
    void projectReloaded(const QString& path);
private slots:
    void onFileChanged(const QString& path);
    void onDebounceTimeout();
private:
    ExternalProjectCache();

    QHash<QString, QSharedPointer<Loaded>> mCache;
    QFileSystemWatcher mWatcher;
    QTimer mDebounce;
    QSet<QString> mPendingPaths;
};

// 时间轴上的"动态链接块"：链接目标是一个外部工程文件（.friction /
// .dreamcut）里的场景。路径 + 场景 documentId + 场景名三元组是事实
// 源（BoxTargetProperty 仅作展示，不参与持久化），装载与重载都由
// ExternalProjectCache 解析出的 Canvas 承载渲染
class CORE_EXPORT InternalLinkCanvasBox : public InternalLinkBox {
    e_OBJECT
    e_DECLARE_TYPE(InternalLinkCanvasBox)
protected:
    InternalLinkCanvasBox();
public:
    void setSourceProject(const QString& path,
                          const int sceneDocId,
                          const QString& sceneName);

    // 源文件变化（或手动）后重新解析并重绑目标场景
    void reloadFromSource();

    const QString& sourcePath() const { return mSourcePath; }
    int sourceSceneDocId() const { return mTargetSceneDocId; }
    const QString& sourceSceneName() const { return mTargetSceneName; }

    void setupCanvasMenu(PropertyMenu * const menu) override;

    void writeBoundingBox(eWriteStream& dst) const override;
    void readBoundingBox(eReadStream& src) override;
protected:
    void bindExternalScene(Canvas * const scene);
    // 块时长跟随外部场景帧范围：初挂建 durRect（0..场景末帧），
    // 重载后范围变化则拉到新末帧（保块头，动画语义=尾部伸缩）
    void syncDurationToTarget();
private:
    QString mSourcePath;
    int mTargetSceneDocId = -1;
    QString mTargetSceneName;
};

#endif // INTERNALLINKCANVASBOX_H
