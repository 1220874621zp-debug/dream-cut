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

#include "projectmanagerdialog.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>

#include "appsupport.h"
#include "themesupport.h"
#include "ReadWrite/evtail.h"

namespace
{
    // 卡片视觉：固定宽 224，缩略图 208x117（16:9），2x DPR
    constexpr int CARD_W = 224;
    constexpr int THUMB_W = 208;
    constexpr int THUMB_H = 117;
    constexpr int GRID_COLUMNS = 4;

    // 旧版缩略图 sidecar（<工程>.dreamcut.png）：仅作读取回退，
    // 新版保存的封面封在工程文件尾部（EvTail）里
    QString thumbPathFor(const QString& projectPath)
    {
        return projectPath + QStringLiteral(".png");
    }

    // 读工程封面：优先工程文件尾部的 EvTail 块，旧工程回退 sidecar
    QImage loadProjectThumb(const QString& projectPath)
    {
        QByteArray pngData;
        EvTail::probePath(projectPath, &pngData);
        QImage img;
        if (!pngData.isEmpty()) {
            img.loadFromData(pngData, "PNG");
        }
        if (img.isNull()) { img.load(thumbPathFor(projectPath)); }
        return img;
    }

    QStringList recentProjects()
    {
        return AppSupport::getSettings(QStringLiteral("files"),
                                       QStringLiteral("recentSaved"))
                .toStringList();
    }

    void setRecentProjects(const QStringList& projects)
    {
        AppSupport::setSettings(QStringLiteral("files"),
                                QStringLiteral("recentSaved"), projects);
    }

    QPixmap placeholderThumb()
    {
        QPixmap pm(THUMB_W * 2, THUMB_H * 2);
        pm.setDevicePixelRatio(2.0);
        pm.fill(QColor(0x23, 0x23, 0x26));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        // 占位图：暗底 + 居中胶片框线条，提示这里还没有缩略图
        p.setPen(QColor(0x55, 0x55, 0x5c));
        p.setBrush(Qt::NoBrush);
        const QRectF r(52, 32, 104, 70);
        p.drawRoundedRect(r, 6, 6);
        QFont f = p.font();
        f.setPixelSize(26);
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("▶"));
        p.end();
        return pm;
    }
}

ProjectManagerDialog::ProjectManagerDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("项目管理"));
    setMinimumSize(880, 540);
    resize(1040, 620);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);

    // 顶部：标题 + 操作按钮
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto* title = new QLabel(tr("项目管理"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto* browseButton = new QPushButton(tr("打开其他工程…"), this);
    auto* newButton = new QPushButton(tr("新建工程"), this);
    newButton->setObjectName(QStringLiteral("projectNewButton"));
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    titleRow->addWidget(browseButton);
    titleRow->addWidget(newButton);
    root->addLayout(titleRow);

    // 卡片滚动区
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    mGridHost = new QWidget(scroll);
    mGrid = new QGridLayout(mGridHost);
    mGrid->setContentsMargins(4, 4, 4, 4);
    mGrid->setHorizontalSpacing(16);
    mGrid->setVerticalSpacing(16);
    mGrid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    scroll->setWidget(mGridHost);
    root->addWidget(scroll, 1);

    mEmptyHint = new QLabel(tr("最近没有工程，点击右上角「新建工程」开始创作"),
                            mGridHost);
    mEmptyHint->setStyleSheet(QStringLiteral("color: #8A8A90;"));
    mGrid->addWidget(mEmptyHint, 0, 0);

    const QString accent = ThemeSupport::getThemeHighlightColor().name();
    const QString accentHover =
            ThemeSupport::getThemeHighlightDarkerColor().name();
    setStyleSheet(QStringLiteral(
        "QFrame#projectCard { background: #232326; border: 1px solid #2E2E36; border-radius: 8px; }"
        "QFrame#projectCard:hover { border: 1px solid %1; }"
        "QPushButton#projectNewButton { background: %1; color: white; border: none; border-radius: 4px; padding: 5px 14px; }"
        "QPushButton#projectNewButton:hover { background: %2; }"
    ).arg(accent, accentHover));

    connect(newButton, &QPushButton::clicked, this, [this]
    {
        done(NewProject);
    });

    connect(browseButton, &QPushButton::clicked, this, [this]
    {
        const QString path = QFileDialog::getOpenFileName(
                    this, tr("打开工程"), QString(),
                    tr("Dream Cut 工程 (*.dreamcut)"));
        if (!path.isEmpty()) { mSelectedProject = path; done(OpenProject); }
    });

    buildCards();
}

bool ProjectManagerDialog::eventFilter(QObject* watched, QEvent* event)
{
    // 整卡点击=打开（卡片自身收到按压；标签子控件不消费按压会自动冒泡到卡）
    if (event->type() == QEvent::MouseButtonPress)
    {
        const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
        const QString path = mCards.key(static_cast<QFrame*>(watched));
        if (mouseEvent->button() == Qt::LeftButton && !path.isEmpty())
        {
            mSelectedProject = path;
            done(OpenProject);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ProjectManagerDialog::buildCards()
{
    const QStringList projects = recentProjects();
    for (const QString& path : projects)
    {
        if (!QFile::exists(path)) { continue; }

        auto* card = new QFrame(mGridHost);
        card->setObjectName(QStringLiteral("projectCard"));
        card->setFixedWidth(CARD_W);
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);

        auto* cardLay = new QVBoxLayout(card);
        cardLay->setContentsMargins(8, 8, 8, 8);
        cardLay->setSpacing(6);

        auto* thumb = new QLabel(card);
        thumb->setObjectName(QStringLiteral("projectThumb"));
        thumb->setFixedSize(THUMB_W, THUMB_H);
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setPixmap(placeholderThumb());
        cardLay->addWidget(thumb, 0, Qt::AlignHCenter);

        const QFileInfo info(path);
        auto* nameRow = new QHBoxLayout;
        nameRow->setSpacing(4);
        auto* nameLabel = new QLabel(info.completeBaseName(), card);
        nameLabel->setToolTip(QDir::toNativeSeparators(info.absoluteFilePath()));
        auto* deleteButton = new QToolButton(card);
        deleteButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
        deleteButton->setToolTip(tr("删除工程（移入回收站）"));
        nameRow->addWidget(nameLabel, 1);
        nameRow->addWidget(deleteButton, 0);
        cardLay->addLayout(nameRow);

        auto* dateLabel = new QLabel(
                    info.lastModified().date().toString(
                        QStringLiteral("yyyy-MM-dd")), card);
        dateLabel->setStyleSheet(QStringLiteral("color: #8A8A90; font-size: 9px;"));
        cardLay->addWidget(dateLabel);

        const int index = mCards.size();
        mGrid->addWidget(card, 1 + index / GRID_COLUMNS, index % GRID_COLUMNS);
        mCards.insert(path, card);

        connect(deleteButton, &QToolButton::clicked, this, [this, path]
        {
            const QMessageBox::StandardButton choice = QMessageBox::warning(
                this, tr("删除工程"),
                tr("将把工程文件移入回收站：\n%1").arg(QDir::toNativeSeparators(path)),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
            if (choice != QMessageBox::Ok) { return; }

            if (!QFile::moveToTrash(path))
            {
                QMessageBox::information(this, tr("删除工程"),
                                         tr("删除失败，文件可能被占用。"));
                return;
            }
            // 缩略图 sidecar 一并清掉（失败不阻塞）
            QFile::moveToTrash(thumbPathFor(path));
            QStringList recents = recentProjects();
            recents.removeAll(path);
            setRecentProjects(recents);
            removeCard(path);
        });

        requestThumbnail(path);
    }
    mEmptyHint->setVisible(mCards.isEmpty());
}

void ProjectManagerDialog::removeCard(const QString& path)
{
    QFrame* card = mCards.take(path);
    if (card == nullptr) { return; }
    card->deleteLater();

    // 按最近列表顺序重排剩余卡片
    const QStringList order = recentProjects();
    for (QFrame* c : mCards)
    {
        mGrid->removeWidget(c);
    }
    int index = 0;
    for (const QString& p : order)
    {
        QFrame* c = mCards.value(p);
        if (c == nullptr) { continue; }
        mGrid->addWidget(c, 1 + index / GRID_COLUMNS, index % GRID_COLUMNS);
        ++index;
    }
    mEmptyHint->setVisible(mCards.isEmpty());
}

void ProjectManagerDialog::requestThumbnail(const QString& path)
{
    QPointer<ProjectManagerDialog> guard(this);
    QThreadPool::globalInstance()->start([guard, path]
    {
        // 封面全尺寸解码后等比缩到 2x 卡片尺寸（新版在工程文件尾部，
        // 旧版在 sidecar）
        QImage source = loadProjectThumb(path);
        QImage thumb;
        if (!source.isNull())
        {
            thumb = source.scaled(THUMB_W * 2, THUMB_H * 2,
                                  Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation);
        }
        if (guard != nullptr)
        {
            QMetaObject::invokeMethod(guard, "onThumbReady",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, path),
                                      Q_ARG(QImage, thumb));
        }
    });
}

void ProjectManagerDialog::onThumbReady(const QString& path,
                                        const QImage& thumb)
{
    QFrame* card = mCards.value(path);
    if (card == nullptr) { return; }

    auto* thumbLabel = card->findChild<QLabel*>(QStringLiteral("projectThumb"));
    if (thumbLabel == nullptr) { return; }

    if (!thumb.isNull())
    {
        // thumb 已是 2x 卡片尺寸：挂上 DPR 后逻辑尺寸正好 208x117
        QPixmap pm = QPixmap::fromImage(thumb);
        pm.setDevicePixelRatio(2.0);
        thumbLabel->setPixmap(pm);
    }
}
