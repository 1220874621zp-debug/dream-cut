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

#ifndef VIDEOBOX_H
#define VIDEOBOX_H
#include <QString>
#include <unordered_map>
#include "Boxes/animationbox.h"
#include "FileCacheHandlers/videocachehandler.h"

class eVideoSound;

class CORE_EXPORT VideoBox : public AnimationBox {
    Q_OBJECT
    e_OBJECT
protected:
    VideoBox();

    void prp_readPropertyXEV_impl(const QDomElement& ele, const XevImporter& imp);
    QDomElement prp_writePropertyXEV_impl(const XevExporter& exp) const;
public:
    struct VideoSpecs {
        QSize dim;
        qreal fps;
        FrameRange range;
    };
    void changeSourceFile();

    void writeBoundingBox(eWriteStream& dst) const;
    void readBoundingBox(eReadStream& src);

    void setStretch(const qreal stretch);

    eVideoSound* sound() const
    { return mSound.get(); }
    void setFilePath(const QString& path);
    QString getFilePath();
    const VideoSpecs getSpecs();
    void setCorrectFps();

    // ---- 代理剪辑（kdenlive 式）----
    // 代理 = 源文件的低清转码副本（540p x264）；只在低分辨率上下
    // 文（播放/预览）接管帧数据，画布静止/快照/导出走原件（分辨率
    // 回 1.0 即自动切回），序列化恒写原路径
    static QString proxyPathFor(const QString& srcPath);
    static bool proxyFileReady(const QString& srcPath);
    static bool proxyPreviewEnabled();
    static void setProxyPreviewEnabled(const bool on);
    bool proxyActive() const { return mProxyActive; }
    // 重判接管条件（分辨率/开关/文件就绪任一变化时调用）
    void updateProxyWiring();
private:
    void setFilePathNoRename(const QString &path);

    void proxyAfterAssigned(VideoFileHandler* obj);

    void soundDataChanged();
    void fileHandlerConnector(ConnContext& conn, VideoFileHandler* obj);
    void fileHandlerAfterAssigned(VideoFileHandler* obj);

    qsptr<eVideoSound> mSound;
    FileHandlerObjRef<VideoFileHandler> mFileHandler;
    // 代理文件句柄（与原件同款包装；仅激活时接管帧数据）
    FileHandlerObjRef<VideoFileHandler> mProxyHandler;
    bool mProxyActive = false;
};

#endif // VIDEOBOX_H
