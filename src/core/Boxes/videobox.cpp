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
# See 'README.md' for more information.
#
*/

// Fork of enve - Copyright (C) 2016-2020 Maurycy Liebner

#include "videobox.h"
#include "ReadWrite/evformat.h"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/imgutils.h>
}

#include <QDebug>
#include <QInputDialog>

#include "Sound/evideosound.h"
#include "canvas.h"
#include "Sound/soundcomposition.h"
#include "filesourcescache.h"
#include "fileshandler.h"
#include "typemenu.h"
#include "appsupport.h"
#include "Private/document.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

VideoFileHandler* videoFileHandlerGetter(const QString& path) {
    return FilesHandler::sInstance->getFileHandler<VideoFileHandler>(path);
}

VideoBox::VideoBox() : AnimationBox("Video", eBoxType::video),
    mFileHandler(this,
                 [](const QString& path) {
                     return videoFileHandlerGetter(path);
                 },
                 [this](VideoFileHandler* obj) {
                     return fileHandlerAfterAssigned(obj);
                 },
                 [this](ConnContext& conn, VideoFileHandler* obj) {
                     fileHandlerConnector(conn, obj);
                 }),
    mProxyHandler(this,
                  [](const QString& path) {
                      return videoFileHandlerGetter(path);
                  },
                  [this](VideoFileHandler* obj) {
                      proxyAfterAssigned(obj);
                  }) {
    const auto flar = getDurationRectangle()->ref<FixedLenAnimationRect>();
    mSound = enve::make_shared<eVideoSound>(flar);
    ca_addChild(mSound);
    //mSound->hide(); // should be on by default
    mSound->SWT_hide();

    connect(this, &eBoxOrSound::parentChanged,
            mSound.get(), &eBoxOrSound::setParentGroup);
    connect(this, &eBoxOrSound::parentChanged,
            this, &VideoBox::setCorrectFps);
    // 代理接管判定的两个触发源：场景分辨率（播放↔静止）与换父
    connect(this, &eBoxOrSound::parentChanged, this, [this]() {
        const auto scene = getParentScene();
        if (!scene) { return; }
        connect(scene, &Canvas::resolutionChanged,
                this, &VideoBox::updateProxyWiring,
                Qt::UniqueConnection);
        updateProxyWiring();
    });
}

void VideoBox::fileHandlerConnector(ConnContext &conn, VideoFileHandler *obj) {
    const auto newDataHandler = obj ? obj->getFrameHandler() : nullptr;
    if(newDataHandler) {
        const auto frameHandler = enve::make_shared<VideoFrameHandler>(newDataHandler);
        setAnimationFramesHandler(frameHandler);
        const auto cacheHandler = &newDataHandler->getCacheHandler();
        getAnimationDurationRect()->setRasterCacheHandler(cacheHandler);
        conn << connect(obj, &VideoFileHandler::pathChanged,
                        this, [this, obj]() {
            fileHandlerAfterAssigned(obj);
        });
        conn << connect(obj, &VideoFileHandler::reloaded,
                        this, &ImageBox::prp_afterWholeInfluenceRangeChanged);
        conn << connect(newDataHandler, &VideoDataHandler::frameCountUpdated,
                        this, &VideoBox::updateAnimationRange);
    }
}

void VideoBox::fileHandlerAfterAssigned(VideoFileHandler *obj) {
    const auto newDataHandler = obj ? obj->getFrameHandler() : nullptr;
    qsptr<AnimationFrameHandler> frameHandler;
    const HddCachableCacheHandler* cacheHandler;
    if(newDataHandler) {
        frameHandler = enve::make_shared<VideoFrameHandler>(newDataHandler);
        cacheHandler = &newDataHandler->getCacheHandler();
    } else cacheHandler = nullptr;
    setAnimationFramesHandler(frameHandler);
    getAnimationDurationRect()->setRasterCacheHandler(cacheHandler);

    soundDataChanged();
    animationDataChanged();
}

void VideoBox::setCorrectFps()
{
    auto const pScene = getParentScene();
    if (!pScene) { return; }

    auto const dataHandler = mFileHandler->getFrameHandler();
    if (!dataHandler) { return; }

    qsptr<VideoFrameHandler> vframeHandler;
    vframeHandler = enve::make_shared<VideoFrameHandler>(dataHandler);

    // Any changes we make to the video in the timeline have to be reflected on the video stream,
    // that's why we have to apply the changes to dataHandler and vframeHandler.
    qreal newmod = (pScene->getFps() /  dataHandler->getFps());

    qDebug() << "VideoBox::setCorrectFps" << newmod;

    dataHandler->setFps(dataHandler->getFps() * newmod);
    vframeHandler->setVideoStreamFps(dataHandler->getFps());

    dataHandler->setFrameCount(vframeHandler->videoStreamFrameCount() * newmod);
    vframeHandler->setVideoStreamFrameCount(vframeHandler->videoStreamFrameCount() * newmod);

    // fps 校正照做（原件 handler 状态），但代理激活时接线归代理
    if (!mProxyActive) {
        setAnimationFramesHandler(vframeHandler);
    }
    animationDataChanged();
}

// ---- 代理剪辑 ----

QString VideoBox::proxyPathFor(const QString& srcPath)
{
    const QString key = QString::fromLatin1(
                QCryptographicHash::hash(
                    QFileInfo(srcPath).absoluteFilePath().toUtf8(),
                    QCryptographicHash::Sha1).toHex().left(16));
    return QStandardPaths::writableLocation(
                QStandardPaths::CacheLocation)
            + QStringLiteral("/proxies/%1_p540.mp4").arg(key);
}

bool VideoBox::proxyFileReady(const QString& srcPath)
{
    const QFileInfo src(srcPath);
    const QFileInfo proxy(proxyPathFor(srcPath));
    return src.exists() && proxy.exists() &&
            proxy.lastModified() >= src.lastModified();
}

bool VideoBox::proxyPreviewEnabled()
{
    return AppSupport::getSettings(QStringLiteral("nle"),
                                   QStringLiteral("proxyPreview"),
                                   false).toBool();
}

void VideoBox::setProxyPreviewEnabled(const bool on)
{
    AppSupport::setSettings(QStringLiteral("nle"),
                            QStringLiteral("proxyPreview"), on);
    // 广播重判：全部场景的全部 VideoBox（NLE 不变量=块是场景直接
    // 子层，一层遍历即可）
    if (!Document::sInstance) { return; }
    for (const auto& scene : Document::sInstance->fScenes) {
        if (!scene) { continue; }
        for (const auto& box : scene->getContained()) {
            const auto vb = enve_cast<VideoBox*>(box.data());
            if (vb) { vb->updateProxyWiring(); }
        }
    }
}

void VideoBox::updateProxyWiring()
{
    const auto scene = getParentScene();
    const bool lowRes = scene && scene->getResolution() < 0.999;
    const bool want = proxyPreviewEnabled() && lowRes &&
            proxyFileReady(mFileHandler.path());
    if (want == mProxyActive) { return; }
    if (want) {
        mProxyActive = true;  // 先置位：assign 回调按它分流
        mProxyHandler.assign(proxyPathFor(mFileHandler.path()));
    } else {
        mProxyActive = false;
        mProxyHandler.assign(QString());  // null → 回原件
    }
}

void VideoBox::proxyAfterAssigned(VideoFileHandler* obj)
{
    if (!mProxyActive || !obj) {
        // 去激活/代理缺失：原件重新接管（声音从未离开原件）
        fileHandlerAfterAssigned(mFileHandler.data());
        return;
    }
    const auto dataHandler = obj->getFrameHandler();
    if (!dataHandler) {
        mProxyActive = false;
        fileHandlerAfterAssigned(mFileHandler.data());
        return;
    }
    const auto frameHandler = enve::make_shared<VideoFrameHandler>(
                dataHandler);
    // fps 校正同原件数学（setCorrectFps）：代理与原件流 fps 相同
    // → 校正后帧数一致，durRect 窗口映射不变
    const auto pScene = getParentScene();
    if (pScene) {
        const qreal newmod = pScene->getFps() / dataHandler->getFps();
        dataHandler->setFps(dataHandler->getFps() * newmod);
        frameHandler->setVideoStreamFps(dataHandler->getFps());
        dataHandler->setFrameCount(
                    frameHandler->videoStreamFrameCount() * newmod);
        frameHandler->setVideoStreamFrameCount(
                    frameHandler->videoStreamFrameCount() * newmod);
    }
    setAnimationFramesHandler(frameHandler);
    getAnimationDurationRect()->setRasterCacheHandler(
                &dataHandler->getCacheHandler());
    animationDataChanged();
}

void VideoBox::writeBoundingBox(eWriteStream& dst) const {
    AnimationBox::writeBoundingBox(dst);
    dst.writeFilePath(mFileHandler->path());
    dst << getStretch();
}

void VideoBox::readBoundingBox(eReadStream& src) {
    AnimationBox::readBoundingBox(src);
    const QString path = src.readFilePath();
    setFilePathNoRename(path);
    if (src.evFileVersion() >= EvFormat::avStretch) {
        qreal stretch;
        src >> stretch;
        setStretch(stretch);
    }
}

QDomElement VideoBox::prp_writePropertyXEV_impl(const XevExporter& exp) const {
    auto result = AnimationBox::prp_writePropertyXEV_impl(exp);
    const QString& absSrc = mFileHandler.path();
    XevExportHelpers::setAbsAndRelFileSrc(absSrc, result, exp);
    return result;
}

void VideoBox::prp_readPropertyXEV_impl(const QDomElement& ele,
                                        const XevImporter& imp) {
    AnimationBox::prp_readPropertyXEV_impl(ele, imp);
    const QString absSrc = XevExportHelpers::getAbsAndRelFileSrc(ele, imp);
    setFilePathNoRename(absSrc);
}

void VideoBox::changeSourceFile()
{
    const QString path = AppSupport::getOpenFile(nullptr,
                                                 tr("Change Source"),
                                                 getFilePath(),
                                                 tr("Video Files (%1)").arg(FileExtensions::videoFilters()));
    if (!path.isEmpty()) { setFilePath(path); }
}

void VideoBox::setStretch(const qreal stretch) {
    AnimationBox::setStretch(stretch);
    mSound->setStretch(stretch);
}

void VideoBox::setFilePathNoRename(const QString &path) {
    mFileHandler.assign(path);
}

void VideoBox::setFilePath(const QString &path) {
    setFilePathNoRename(path);
    rename(QFileInfo(path).completeBaseName());
}

QString VideoBox::getFilePath() {
    return mFileHandler.path();
}

const VideoBox::VideoSpecs VideoBox::getSpecs()
{
    VideoSpecs specs;
    const auto h = mFileHandler->getFrameHandler();
    if (!h) { return specs; }
    int frames = h->getFrameCount();
    specs.range = FrameRange{0, frames <= 1 ? 0 : frames - 1};
    specs.fps = h->getFps();
    specs.dim = h->getDim();
    return specs;
}

void VideoBox::soundDataChanged()
{
    const auto pScene = getParentScene();
    const auto soundHandler = mFileHandler ?
                mFileHandler->getSoundHandler() : nullptr;
    const auto durRect = getDurationRectangle();
    if (soundHandler) {
        if (!mSound->SWT_isVisible()) {
            if (pScene) { pScene->getSoundComposition()->addSound(mSound); }
        }
        durRect->setSoundCacheHandler(&soundHandler->getCacheHandler());
    } else {
        if (mSound->SWT_isVisible()) {
            if (pScene) { pScene->getSoundComposition()->removeSound(mSound); }
        }
        durRect->setSoundCacheHandler(nullptr);
    }
    mSound->setSoundDataHandler(soundHandler);
    mSound->SWT_setVisible(soundHandler);

    // why? this does not make any sense to me
    //mSound->setVisible(soundHandler);
}
