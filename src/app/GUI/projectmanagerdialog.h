/*
#
# Dream Cut - project manager panel
#
# Based on the ProjectManagerDialog of the Pencil Dream fork,
# rebuilt for Dream Cut (.dreamcut projects, sidecar PNG thumbs).
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3.
#
*/

#ifndef PROJECTMANAGERDIALOG_H
#define PROJECTMANAGERDIALOG_H

#include <QDialog>
#include <QFrame>
#include <QMap>

class QGridLayout;
class QLabel;

// 项目管理面板：最近工程卡片（16:9 缩略图/名称/修改日期），整卡点击
// =打开，卡上删除钮=移入回收站并移出最近列表；顶部支持新建/浏览打开。
// 启动时自动弹出（见 MainWindow::readSettings 尾部），文件菜单可随时再开
class ProjectManagerDialog : public QDialog
{
    Q_OBJECT

public:
    enum ResultCode
    {
        OpenProject = 2, // 非标准 QDialog 返回值，区分"打开了某工程"
        NewProject = 3,
    };

    explicit ProjectManagerDialog(QWidget* parent = nullptr);

    // ResultCode::OpenProject 时有效
    QString selectedProject() const { return mSelectedProject; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onThumbReady(const QString& path, const QImage& thumb);

private:
    void buildCards();
    void removeCard(const QString& path);
    void requestThumbnail(const QString& path);

    QString mSelectedProject;
    QGridLayout* mGrid = nullptr;
    QWidget* mGridHost = nullptr;
    QLabel* mEmptyHint = nullptr;
    QMap<QString, QFrame*> mCards;
};

#endif // PROJECTMANAGERDIALOG_H
