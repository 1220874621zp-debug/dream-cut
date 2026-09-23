#include "nletimelineview.h"
#include "nletimelinemodel.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QMenu>
#include <QContextMenuEvent>
#include <QtMath>
#include <QRandomGenerator>
#include <QDebug>
#include <QCursor>
#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QDateTime>
#include <QWidgetAction>
#include <QPointer>
#include <functional>
#include <algorithm>
#include <climits>

#include "themesupport.h"
#include <QSvgRenderer>
#include <QHash>
#include "Boxes/videobox.h"
#include "Sound/evideosound.h"
#include "Sound/eindependentsound.h"
#include "Animators/qrealkey.h"
#include <QFileInfo>

static const int SNAP_PX = 8;
static const int TRIM_PX = 6;
// kdenlive MouseArea drag.threshold equivalent: presses stay clicks
// until the pointer crosses this many pixels
static const int DRAG_THRESHOLD_PX = 5;

NleTimelineView::NleTimelineView(NleTimelineModel * const model,
                                 QWidget * const parent)
    : QWidget(parent)
    , mModel(model)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(320);

    // repaint on any model change (tracks/clips/selection); the media
    // caches survive because clip ids are stable per layer - dead ids
    // (deleted clips, reloaded projects) are pruned on each change
    connect(mModel, &NleTimelineModel::modelChanged,
            this, [this]() { pruneMediaCaches(); updateScrollBar(); update(); });
    connect(mModel, &NleTimelineModel::selectionChanged,
            this, [this]() { update(); });
    // 颜色标记/停用态变化：轻量重绘
    connect(mModel, &NleTimelineModel::clipDecorationsChanged,
            this, [this]() { update(); });

    // procedural blade cursor for the razor tool (PR-style): a red
    // blade with a grey handle, hotspot on the blade tip
    {
        const qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(24, 24) * dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(dpr, dpr); // painter works in physical pixels
        p.setPen(QPen(QColor(0xe8, 0x4c, 0x4c), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5, 19), QPointF(14, 10));
        p.setPen(QPen(QColor(0xc8, 0xc8, 0xc8), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(14, 10), QPointF(20, 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe8, 0x4c, 0x4c));
        p.drawEllipse(QPointF(7.5, 16.5), 1.6, 1.6);
        p.end();
        pm.setDevicePixelRatio(dpr);
        mRazorCursor = QCursor(pm, 5, 19);
    }
}

void NleTimelineView::setScrollBar(QScrollBar * const bar)
{
    mScrollBar = bar;
    if (!mScrollBar) { return; }
    connect(mScrollBar, &QScrollBar::valueChanged, this, [this](const int v) {
        mScrollFrame = v;
        update();
        emit viewChanged();
    });
    updateScrollBar();
}

// ---------------------------------------------------------------- mapping

int NleTimelineView::trackHeight(const int trackIdx) const
{
    const auto &tracks = mModel->tracks();
    const int h = tracks.value(trackIdx).height;
    const int base = h > 0 ? h : (tracks.value(trackIdx).audio ? 52 : 60);
    return mHeightPreview.value(tracks.value(trackIdx).id, base);
}

int NleTimelineView::trackY(const int trackIdx) const
{
    int y = rulerHeight();
    for (int i = 0; i < trackIdx && i < mModel->tracks().size(); ++i) {
        y += trackHeight(i);
    }
    return y;
}

int NleTimelineView::trackAtY(const int y) const
{
    if (y < rulerHeight()) { return -1; }
    const int n = mModel->tracks().size();
    for (int i = 0; i < n; ++i) {
        const int top = trackY(i);
        if (y >= top && y < top + trackHeight(i)) { return i; }
    }
    return -1;
}

int NleTimelineView::xToFrame(const int x) const
{
    return mScrollFrame + qRound((x - headerWidth()) / mPxPerFrame);
}

int NleTimelineView::frameToX(const int frame) const
{
    return headerWidth() + qRound((frame - mScrollFrame) * mPxPerFrame);
}

int NleTimelineView::contentFrames() const
{
    const qreal fps = mModel->fps();
    int end = qRound(30.0 * fps);
    for (const auto &c : mModel->clips()) {
        end = qMax(end, c.start + c.duration + qRound(5.0 * fps));
    }
    return end;
}

QRectF NleTimelineView::moveRect(const NleTimelineModel::Move &m) const
{
    const double x = frameToX(m.start);
    const double w = m.duration * mPxPerFrame;
    int top;
    if (m.trackId == kGhostTrackId) {
        top = ghostLaneTop() + 2;
    } else {
        top = trackY(mModel->trackIndex(m.trackId)) + 2;
    }
    const int idx = mModel->trackIndex(m.trackId);
    const int h = (idx >= 0 ? trackHeight(idx)
                            : (mGhostLaneAudio ? 52 : 60)) - 4;
    return QRectF(x, top, w, h);
}

// y of the lane a to-be-created track will occupy: after the last
// track of its type (the panel renders video specs first, then audio
// specs - a new video lane grows at the bottom of the video group)
int NleTimelineView::ghostLaneTop() const
{
    const auto &tracks = mModel->tracks();
    int idx = 0;
    if (mGhostLaneTop) {
        // the group head: above V1 for video (the very top), above
        // the audio group for audio (right below the video group)
        while (idx < tracks.size() &&
               tracks.at(idx).audio != mGhostLaneAudio) { ++idx; }
        return trackY(idx);
    }
    while (idx < tracks.size() &&
           tracks.at(idx).audio != mGhostLaneAudio) { ++idx; }
    while (idx < tracks.size() &&
           tracks.at(idx).audio == mGhostLaneAudio) { ++idx; }
    return trackY(idx);
}

QRectF NleTimelineView::clipRect(const NleTimelineModel::Clip &c) const
{
    return moveRect({c.clipId, c.trackId, c.start, c.duration});
}

NleTimelineModel::Move NleTimelineView::effectiveMove(const int clipId) const
{
    const auto it = mCandidates.constFind(clipId);
    if (it != mCandidates.constEnd()) { return it.value(); }
    const auto c = mModel->clip(clipId);
    if (c) { return {c->clipId, c->trackId, c->start, c->duration}; }
    return {};
}

// ---------------------------------------------------------------- painting

void NleTimelineView::paintEvent(QPaintEvent *)
{
    refreshThemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), cBg);

    drawTrackBodies(p);
    drawRuler(p);
    drawTrackHeaders(p);

    // clips (model order; candidates override the moving ones)
    for (const auto &c : mModel->clips()) {
        drawClip(p, c);
    }

    drawSeamDots(p);

    // ghost of the primary dragged clip at its original position
    if (mDrag == DragMode::MoveClip && mDragClipId >= 0) {
        const auto c = mModel->clip(mDragClipId);
        const auto cand = mCandidates.constFind(mDragClipId);
        if (c && cand != mCandidates.constEnd() &&
                (cand.value().start != c->start ||
                 cand.value().trackId != c->trackId)) {
            drawClip(p, *c, true);
        }
    }

    // snap guide line
    if (mSnapTarget >= 0) {
        const int x = frameToX(mSnapTarget);
        p.setPen(QPen(QColor(0xff, 0xd1, 0x54), 1, Qt::DashLine));
        p.drawLine(x, rulerHeight(), x, height());
    }

    // CapCut lane lifecycle preview: the dashed lane a release below
    // every track will create (with the drop hint)
    if (mGhostLane && mDrag == DragMode::MoveClip) {
        const int top = ghostLaneTop();
        const int h = mGhostLaneAudio ? 52 : 60;
        const QRect band(headerWidth(), top, width() - headerWidth(), h);
        p.fillRect(band, QColor(0x1e, 0x24, 0x22));
        p.setPen(QPen(cAccent, 1, Qt::DashLine));
        p.drawRect(band.adjusted(0, 0, -1, -1));
        p.setPen(cAccent);
        QFont gf = font();
        gf.setPixelSize(10);
        p.setFont(gf);
        p.drawText(band.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   tr("松开创建新轨道"));
    }

    // rubber band selection overlay
    if (mRubber) {
        const QRect band = QRect(mRubberStart, mapFromGlobal(QCursor::pos()))
                .normalized().intersected(rect().adjusted(headerWidth(), rulerHeight(), -1, -1));
        if (!band.isEmpty()) {
            p.setPen(QPen(cAccent, 1, Qt::DashLine));
            p.setBrush(QColor(cAccent.red(), cAccent.green(), cAccent.blue(), 30));
            p.drawRect(band);
        }
    }

    // razor guide: dashed cut preview under the cursor across the
    // whole content area (matches the playhead red)
    if (mTool == EditTool::Razor && mDrag == DragMode::None &&
            mHoverPos.x() >= headerWidth() && mHoverPos.y() > rulerHeight()) {
        const int rx = mHoverPos.x();
        p.setPen(QPen(cPlayhead, 1, Qt::DashLine));
        p.drawLine(rx, rulerHeight(), rx, height());
    }

    drawPlayhead(p);

    // corner between ruler and headers
    p.fillRect(0, 0, headerWidth(), rulerHeight(), cHeader);
    p.setPen(cGridLine);
    p.drawLine(0, rulerHeight() - 1, width(), rulerHeight() - 1);

    // corner +V / +A buttons: explicit track creation
    QFont cf = font();
    cf.setPixelSize(10);
    p.setFont(cf);
    for (int a = 0; a < 2; ++a) {
        const bool audio = a == 1;
        const QRect b = addTrackRect(audio);
        const bool hov = b.contains(mHoverPos);
        p.setPen(QPen(hov ? cAccent : cGridLine, 1));
        p.setBrush(hov ? QColor(cAccent.red(), cAccent.green(),
                                cAccent.blue(), 46)
                       : QColor(0x2a, 0x2a, 0x2c));
        p.drawRoundedRect(b, 3, 3);
        p.setPen(hov ? QColor(0xff, 0xff, 0xff) : cText);
        p.drawText(b, Qt::AlignCenter,
                   audio ? QStringLiteral("+A") : QStringLiteral("+V"));
    }
}

void NleTimelineView::drawRuler(QPainter &p)
{
    p.fillRect(headerWidth(), 0, width() - headerWidth(), rulerHeight(), cRuler);

    // scene in/out band (dim stripe over the whole timeline height)
    if (mRangeIn >= 0 && mRangeOut > mRangeIn) {
        const int x0 = qMax(headerWidth(), frameToX(mRangeIn));
        const int x1 = qMin(width(), frameToX(mRangeOut));
        if (x1 > x0) {
            p.fillRect(QRect(x0, rulerHeight(), x1 - x0,
                             height() - rulerHeight()),
                       QColor(0xff, 0xd1, 0x54, 14));
            p.setPen(QPen(QColor(0xff, 0xd1, 0x54, 120), 1));
            p.drawLine(x0, rulerHeight(), x0, height());
            p.drawLine(x1, rulerHeight(), x1, height());
        }
    }

    // choose a tick step (whole seconds ladder) so labels stay readable
    const qreal fps = qMax(1.0, mModel->fps());
    static const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60, 300};
    int stepFrames = qRound(fps);
    for (const double s : steps) {
        const int f = qRound(s * fps);
        if (f >= 1 && f * mPxPerFrame >= 70) { stepFrames = f; break; }
    }

    p.setFont(font());
    const int first = (qMax(0, mScrollFrame - stepFrames) / stepFrames) * stepFrames;
    for (int frame = first; ; frame += stepFrames) {
        const int x = frameToX(frame);
        if (x > width() + 4) { break; }
        if (x < headerWidth()) { continue; }
        p.setPen(cGridLine);
        p.drawLine(x, rulerHeight() - 8, x, rulerHeight());
        p.setPen(cTextDim);
        p.drawText(QRect(x + 3, 2, 90, rulerHeight() - 10),
                   Qt::AlignLeft | Qt::AlignVCenter, timecode(frame));
        // minor ticks
        for (int k = 1; k < 4; ++k) {
            const int mx = frameToX(frame + stepFrames * k / 4);
            if (mx <= width()) {
                p.setPen(QColor(0x26, 0x26, 0x26));
                p.drawLine(mx, rulerHeight() - 4, mx, rulerHeight());
            }
        }
    }

    // scene markers: amber guides through the whole timeline
    for (const auto &mark : mMarkers) {
        const int x = frameToX(mark.first);
        if (x < headerWidth() || x > width()) { continue; }
        p.setPen(QPen(QColor(0xff, 0xd1, 0x54), 1, Qt::DashLine));
        p.drawLine(x, 0, x, height());
        if (!mark.second.isEmpty()) {
            p.setPen(QColor(0xff, 0xd1, 0x54));
            p.drawText(QRect(x + 2, rulerHeight() - 1, 120, 14),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(mark.second, Qt::ElideRight, 116));
        }
    }
}

// 剪映同款轨道头线性图标：16x16 视图框，颜色烤进 SVG 后按 key+色缓存
static QPixmap nleHeaderGlyph(const QString &key, const QByteArray &svgBody,
                              const QColor &color, const int size) {
    static QHash<QString, QPixmap> cache;
    const QString cacheKey = key + QLatin1Char('|') + color.name()
            + QLatin1Char('|') + QString::number(size);
    const auto hit = cache.constFind(cacheKey);
    if (hit != cache.constEnd()) { return hit.value(); }
    const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "viewBox=\"0 0 16 16\">" + svgBody + "</svg>";
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QSvgRenderer renderer(svg);
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();
    cache.insert(cacheKey, pm);
    return pm;
}

static QByteArray nleGlyphBody(const QString &key, const bool off,
                               const QColor &c) {
    const QString col = c.name();
    QByteArray body;
    QByteArray strokeCol = QString("stroke=\"%1\" stroke-width=\"1.4\" "
                                   "stroke-linecap=\"round\" "
                                   "stroke-linejoin=\"round\" fill=\"none\"")
                                   .arg(col).toUtf8();
    if (key == "video") {
        // 视频轨：圆角片框 + 播放三角
        body = "<rect x=\"1.7\" y=\"3.2\" width=\"12.6\" height=\"9.6\" "
               "rx=\"1.8\" " + strokeCol + "/>"
               "<path d=\"M6.7 5.9 L10.4 8 L6.7 10.1 Z\" fill=\"" + col.toUtf8() + "\"/>";
    } else if (key == "music") {
        // 音频轨：音符
        body = "<circle cx=\"6\" cy=\"11.2\" r=\"2\" fill=\"" + col.toUtf8() + "\"/>"
               "<path d=\"M8 11.2 V3.9 C 8.6 5.8 10.3 6.3 11.6 5.9\" " + strokeCol + "/>";
    } else if (key == "eye") {
        body = "<path d=\"M1.8 8 C 3.5 4.9 5.6 3.4 8 3.4 C 10.4 3.4 12.5 4.9 14.2 8 "
               "C 12.5 11.1 10.4 12.6 8 12.6 C 5.6 12.6 3.5 11.1 1.8 8 Z\" " + strokeCol + "/>"
               "<circle cx=\"8\" cy=\"8\" r=\"1.9\" fill=\"" + col.toUtf8() + "\"/>";
        if (off) { body += "<path d=\"M2.6 13.4 L13.4 2.6\" stroke=\"" + col.toUtf8() +
                           "\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>";
        }
    } else if (key == "speaker") {
        body = "<path d=\"M2.2 6.2 H4.6 L7.6 3.6 V12.4 L4.6 9.8 H2.2 Z\" fill=\"" +
               col.toUtf8() + "\"/>";
        if (!off) {
            body += "<path d=\"M10 5.8 A 3.1 3.1 0 0 1 10 10.2\" " + strokeCol + "/>"
                    "<path d=\"M12.1 4.2 A 5.4 5.4 0 0 1 12.1 11.8\" " + strokeCol + "/>";
        } else {
            body += "<path d=\"M2.6 13.4 L13.4 2.6\" stroke=\"" + col.toUtf8() +
                    "\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>";
        }
    } else if (key == "lock") {
        body = "<rect x=\"3.6\" y=\"7.2\" width=\"8.8\" height=\"6\" rx=\"1.5\" " +
               strokeCol + "/>";
        body += off
            ? "<path d=\"M5.6 7.2 V5.4 A 2.4 2.4 0 0 1 10.4 5.4\" " + strokeCol + "/>"
            : "<path d=\"M5.6 7.2 V5.4 A 2.4 2.4 0 0 1 10.4 5.4 V7.2\" " + strokeCol + "/>";
    }
    return body;
}

void NleTimelineView::drawTrackHeaders(QPainter &p)
{
    const auto &tracks = mModel->tracks();
    // CapCut lane numbers: video lanes count up from the main track
    // (1 sits right above it, the header column reads n..1 top-down),
    // audio lanes count up from the bottom; the main track carries
    // the badge instead of a number
    const int mainId = mModel->mainTrackId();
    QHash<int, int> laneNo;
    {
        int above = 0;
        for (int i = tracks.size() - 1; i >= 0; --i) {
            const auto &t = tracks[i];
            if (t.audio || t.id == mainId) { continue; }
            laneNo.insert(t.id, ++above);
        }
        int aNo = 0;
        for (int i = tracks.size() - 1; i >= 0; --i) {
            if (!tracks[i].audio) { continue; }
            laneNo.insert(tracks[i].id, ++aNo);
        }
    }
    for (int i = 0; i < tracks.size(); ++i) {
        const int top = trackY(i);
        const int h = trackHeight(i);
        p.fillRect(0, top, headerWidth(), h, cHeader);
        p.setPen(cGridLine);
        p.drawLine(0, top + h - 1, headerWidth(), top + h - 1);
        p.drawLine(headerWidth() - 1, top, headerWidth() - 1, top + h);

        // 剪映同款：轨道类型线性图标（视频=片框播放 / 音频=音符）
        const bool audio = tracks[i].audio;
        const QString typeKey = audio ? QStringLiteral("music")
                                      : QStringLiteral("video");
        p.drawPixmap(10, top + (h - 18) / 2,
                     nleHeaderGlyph(typeKey,
                                    nleGlyphBody(typeKey, false, cText),
                                    cText, 18));
        // CapCut main track: the bottom video lane carries the badge
        // and drives the magnetic layout + overlay following
        if (!audio && tracks[i].id == mainId) {
            QFont mf = font();
            mf.setPixelSize(9);
            p.setFont(mf);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xff, 0xd1, 0x54));
            const QRect mb(34, top + (h - 16) / 2, 14, 16);
            p.drawRoundedRect(mb, 3, 3);
            p.setPen(QColor(0x33, 0x28, 0x08));
            p.drawText(mb, Qt::AlignCenter, QStringLiteral("主"));
        } else {
            // CapCut lane number next to the type glyph (V-n / A-n,
            // stored per-type so both stacks count from the main row)
            const int no = laneNo.value(tracks[i].id, -1);
            if (no > 0) {
                QFont nf = font();
                nf.setPixelSize(9);
                p.setFont(nf);
                p.setPen(cTextDim);
                p.drawText(QRect(30, top, 18, h), Qt::AlignVCenter,
                           QStringLiteral("%1%2")
                               .arg(audio ? QStringLiteral("A")
                                          : QStringLiteral("V"))
                               .arg(no));
            }
        }

        p.setPen(tracks[i].muted ? cTextDim : cText);
        p.drawText(QRect(52, top, headerWidth() - 144, h),
                   Qt::AlignVCenter, tracks[i].name);

        // 剪映顺序：锁在左，眼(视频)/喇叭(音频)在右
        const QRect lb = lockBadgeRect(i);
        {
            const bool locked = tracks[i].locked;
            const QColor lc = locked ? QColor(0xff, 0xd1, 0x54) : cTextDim;
            p.drawPixmap(lb.topLeft(),
                         nleHeaderGlyph(QStringLiteral("lock"),
                                        nleGlyphBody(QStringLiteral("lock"),
                                                     !locked, lc),
                                        lc, lb.width()));
        }

        const QRect mb = muteBadgeRect(i);
        const bool laneMuted = tracks[i].muted;
        const QString ctrlKey = audio ? QStringLiteral("speaker")
                                      : QStringLiteral("eye");
        const QColor mc = laneMuted ? QColor(0xe8, 0x4c, 0x4c) : cText;
        p.drawPixmap(mb.topLeft(),
                     nleHeaderGlyph(ctrlKey,
                                    nleGlyphBody(ctrlKey, laneMuted, mc),
                                    mc, mb.width()));

        // 剪映式独奏 S：未独奏灰、独奏金（同锁定的激活色语言）
        const QRect sb = soloBadgeRect(i);
        QFont sf = font();
        sf.setBold(true);
        sf.setPixelSize(12);
        p.setFont(sf);
        p.setPen(tracks[i].solo ? QColor(0xff, 0xd1, 0x54) : cTextDim);
        p.drawText(sb, Qt::AlignCenter, QStringLiteral("S"));
    }
}

void NleTimelineView::drawTrackBodies(QPainter &p)
{
    for (int i = 0; i < mModel->tracks().size(); ++i) {
        const int top = trackY(i);
        const int h = trackHeight(i);
        p.fillRect(headerWidth(), top, width() - headerWidth(), h,
                   (i % 2) ? cBgAlt : cBg);
        p.setPen(QColor(0x26, 0x26, 0x26));
        p.drawLine(headerWidth(), top + h - 1, width(), top + h - 1);
    }
}

void NleTimelineView::drawSeamDots(QPainter &p)
{
    // CapCut seam marks: a red dot wherever two neighbouring main
    // clips butt together. Placements go through effectiveMove so the
    // dots ride a running drag (the magnetic plan shifts the seams)
    const int mainId = mModel->mainTrackId();
    const int idx = mModel->trackIndex(mainId);
    if (idx < 0) { return; }
    QVector<QPair<int, int>> spans; // {start, end}
    for (const auto &c : mModel->clips()) {
        if (c.trackId != mainId) { continue; }
        const auto m = effectiveMove(c.clipId);
        spans.append({m.start, m.start + m.duration});
    }
    if (spans.size() < 2) { return; }
    std::sort(spans.begin(), spans.end());
    const qreal cy = trackY(idx) + trackHeight(idx) / 2.;
    p.setPen(QPen(QColor(0x10, 0x18, 0x20), 1));
    p.setBrush(QColor(0xe8, 0x4c, 0x4c));
    for (int i = 1; i < spans.size(); ++i) {
        if (spans[i - 1].second != spans[i].first) { continue; }
        const qreal x = frameToX(spans[i].first);
        if (x < headerWidth() || x > width()) { continue; }
        p.drawEllipse(QPointF(x, cy), 3., 3.);
    }
}

// volume 0..200 (percent) -> wave-body y: 100 = 0dB midline, the
// full range maps bottom..top (CapCut volume envelope geometry)
namespace {
qreal nleVolY(const QRectF &body, const qreal vol)
{
    return body.top() + body.height() *
            (1. - qBound(0., vol, 200.) / 200.);
}
} // namespace

void NleTimelineView::drawVolumeEnvelope(QPainter &p,
                                         const NleTimelineModel::Clip &c,
                                         const QRectF &body)
{
    const auto sound = enve_cast<eSound*>(c.layer.data());
    if (!sound) { return; }
    auto * const anim = sound->volumeAnimator();
    if (!anim || body.height() < 12 || body.width() < 8) { return; }
    // candidate-aware so the envelope rides a running drag
    const auto m = effectiveMove(c.clipId);
    const double x0 = frameToX(m.start);

    p.save();
    p.setClipRect(body.adjusted(0, -2, 0, 2), Qt::IntersectClip);
    // 0dB reference midline
    p.setPen(QPen(QColor(0xff, 0xff, 0xff, 56), 1, Qt::DotLine));
    p.drawLine(QPointF(body.left(), nleVolY(body, 100)),
               QPointF(body.right(), nleVolY(body, 100)));
    // envelope polyline sampled from the animator
    p.setPen(QPen(QColor(0xff, 0xff, 0xff, 220), 1.5));
    QPainterPath line;
    bool started = false;
    for (int x = qRound(body.left()); x <= qRound(body.right()); x += 2) {
        const qreal rel = (x - x0) / mPxPerFrame;
        const QPointF pt(x, nleVolY(body, anim->getBaseValue(rel)));
        if (!started) { line.moveTo(pt); started = true; }
        else { line.lineTo(pt); }
    }
    p.drawPath(line);
    // key dots
    p.setPen(QPen(QColor(0x18, 0x2a, 0x40), 1));
    p.setBrush(QColor(0xff, 0xff, 0xff));
    for (const auto &k : anim->anim_getKeys()) {
        const auto qk = static_cast<QrealKey*>(k);
        const qreal kx = x0 + qk->getRelFrame() * mPxPerFrame;
        if (kx < body.left() - 4 || kx > body.right() + 4) { continue; }
        p.drawEllipse(QPointF(kx, nleVolY(body, qk->getValue())), 2.6, 2.6);
    }
    p.restore();
}

QString NleTimelineView::audioTagFor(const NleTimelineModel::Clip &c)
{
    const auto it = mAudioTags.constFind(c.clipId);
    if (it != mAudioTags.constEnd()) { return it.value(); }
    QString tag;
    // bitrate only for independent sounds: an embedded/separated
    // video sound would report the whole container's size
    const auto indep = enve_cast<eIndependentSound*>(c.layer.data());
    if (indep) {
        const QFileInfo fi(indep->getFilePath());
        if (fi.exists() && c.duration > 0) {
            const qreal secs = qMax(0.5, c.duration /
                                    qMax(1., mModel->fps()) / qMax(0.01, c.speed));
            const int kbps = qRound(fi.size() * 8 / secs / 1000);
            if (kbps > 0) { tag = QStringLiteral("%1kbps").arg(kbps); }
        }
    }
    mAudioTags.insert(c.clipId, tag);
    return tag;
}

NleTimelineView::VolHit NleTimelineView::volumeHitTest(
        const QPoint &pos, int *const clipIdOut,
        QrealKey **const keyOut) const
{
    if (clipIdOut) { *clipIdOut = -1; }
    if (keyOut) { *keyOut = nullptr; }
    const int clipId = clipAt(pos);
    if (clipId < 0) { return VolHit::None; }
    const auto c = mModel->clip(clipId);
    if (!c || !c->audio) { return VolHit::None; }
    const auto sound = enve_cast<eSound*>(c->layer.data());
    auto * const anim = sound ? sound->volumeAnimator() : nullptr;
    if (!anim) { return VolHit::None; }
    const auto m = effectiveMove(clipId);
    // same body geometry the envelope is painted into (nameBarH = 16)
    const QRectF body = moveRect(m).adjusted(2, 18, -2, -3);
    if (!body.contains(pos)) { return VolHit::None; }
    const double x0 = frameToX(m.start);
    QrealKey *best = nullptr;
    qreal bestDx = 8;
    for (const auto &k : anim->anim_getKeys()) {
        const auto qk = static_cast<QrealKey*>(k);
        const qreal dx = qAbs(x0 + qk->getRelFrame() * mPxPerFrame
                              - pos.x());
        if (dx < bestDx) { bestDx = dx; best = qk; }
    }
    if (clipIdOut) { *clipIdOut = clipId; }
    if (best && qAbs(nleVolY(body, best->getValue()) - pos.y()) <= 7) {
        if (keyOut) { *keyOut = best; }
        return VolHit::Key;
    }
    const qreal rel = (pos.x() - x0) / mPxPerFrame;
    const qreal y = nleVolY(body, anim->getBaseValue(rel));
    return qAbs(y - pos.y()) <= 6 ? VolHit::Line : VolHit::None;
}

// arm (or complete, for Alt+key delete) a volume-point gesture;
// returns true when the press was consumed by the envelope
bool NleTimelineView::armVolumeGesture(const QPoint &pos, const bool alt)
{
    int clipId = -1;
    QrealKey *key = nullptr;
    const VolHit hit = volumeHitTest(pos, &clipId, &key);
    if (hit == VolHit::None) { return false; }
    const auto c = mModel->clip(clipId);
    const auto sound = enve_cast<eSound*>(c ? c->layer.data() : nullptr);
    auto * const anim = sound ? sound->volumeAnimator() : nullptr;
    if (!anim) { return false; }
    if (hit == VolHit::Key) {
        if (alt) {
            anim->anim_removeKeyAction(key->ref<Key>());
            emit logMessage(tr("已删除音量关键点"));
            update();
            return true;
        }
        mVolAnim = anim;
        mVolKey = key;
        mVolClipId = clipId;
        mDrag = DragMode::VolumePoint;
        return true;
    }
    // line hit: drop a key at the cursor frame holding the current
    // interpolated value (undoable), then drag it
    const auto m = effectiveMove(clipId);
    const int relF = qRound((pos.x() - frameToX(m.start)) / mPxPerFrame);
    const qreal val = anim->getBaseValue(relF);
    const auto newKey = enve::make_shared<QrealKey>(val, m.start + relF,
                                                    anim);
    anim->anim_appendKeyAction(newKey);
    mVolAnim = anim;
    mVolKey = newKey.get();
    mVolClipId = clipId;
    mDrag = DragMode::VolumePoint;
    return true;
}

QString NleTimelineView::timecode(const int frame) const
{
    const int fpsInt = qMax(1, qRound(mModel->fps()));
    const int ffTot = frame;
    const int mm = ffTot / (60 * fpsInt);
    const int ss = (ffTot / fpsInt) % 60;
    const int ff = ffTot % fpsInt;
    return QStringLiteral("%1:%2:%3")
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

QPixmap NleTimelineView::thumbnailTile(const int clipId, const int hueSeed,
                                       const int h)
{
    // real rendered frame first: scaled to the strip height once,
    // then repeated across the clip body (midpoint WYSIWYG renders
    // for non-video visual layers)
    const auto raw = mRealThumbs.constFind(clipId);
    if (raw != mRealThumbs.constEnd() && !raw.value().isNull()) {
        auto scaled = mRealScaled.constFind(clipId);
        if (scaled == mRealScaled.constEnd() || scaled.value().height() != h) {
            const QPixmap pm = QPixmap::fromImage(
                        raw.value().scaledToHeight(h, Qt::SmoothTransformation));
            if (scaled != mRealScaled.constEnd()) { mRealScaled[clipId] = pm; }
            else { mRealScaled.insert(clipId, pm); }
            return pm;
        }
        return scaled.value();
    }
    // procedural placeholder tile: one per clip, repeated across the
    // clip body
    const int tileW = qMax(48, int(h * 16.0 / 9.0));
    const QString key = QStringLiteral("%1x%2").arg(clipId).arg(h);
    const auto it = mThumbCache.constFind(key);
    if (it != mThumbCache.constEnd()) { return it.value(); }

    QPixmap pm(tileW, h);
    pm.fill(Qt::transparent);
    QPainter tp(&pm);
    tp.setRenderHint(QPainter::Antialiasing);

    QRandomGenerator rng{quint32(hueSeed)};
    // base gradient, hue varies per clip
    const int hue = (hueSeed * 47) % 360;
    const QColor base = QColor::fromHsv(hue, 110, 150);
    const QColor base2 = QColor::fromHsv((hue + 40) % 360, 130, 110);
    QLinearGradient g(0, 0, tileW, h);
    g.setColorAt(0, base);
    g.setColorAt(1, base2);
    tp.fillRect(pm.rect(), g);

    // scenery-ish shapes so tiles look like frames
    for (int i = 0; i < 4; ++i) {
        const int w = 10 + rng.bounded(tileW / 2);
        const int hh = 6 + rng.bounded(h / 2);
        const int x = rng.bounded(qMax(1, tileW - w));
        const int y = rng.bounded(qMax(1, h - hh));
        QColor c2 = QColor::fromHsv((hue + rng.bounded(120)) % 360,
                                    60 + rng.bounded(120), 90 + rng.bounded(120));
        c2.setAlpha(150);
        tp.setPen(Qt::NoPen);
        tp.setBrush(c2);
        if (rng.bounded(2)) { tp.drawEllipse(x, y, w, hh); }
        else { tp.drawRoundedRect(x, y, w, hh, 3, 3); }
    }
    // silhouette horizon
    tp.setBrush(QColor(0, 0, 0, 90));
    QPolygonF hill;
    hill << QPointF(0, h);
    for (int x = 0; x <= tileW; x += 8) {
        hill << QPointF(x, h - 6 - rng.bounded(h / 3));
    }
    hill << QPointF(tileW, h);
    tp.drawPolygon(hill);

    // vignette
    QLinearGradient v(0, 0, 0, h);
    v.setColorAt(0, QColor(255, 255, 255, 26));
    v.setColorAt(0.5, QColor(0, 0, 0, 0));
    v.setColorAt(1, QColor(0, 0, 0, 70));
    tp.fillRect(pm.rect(), v);
    tp.end();

    mThumbCache.insert(key, pm);
    return pm;
}

// centered waveform strip: real peaks when the controller fed them
// (clip id keyed), deterministic pseudo wave otherwise (audio-only
// clips before their media lands)
void NleTimelineView::drawWave(QPainter &p, const QRectF &body,
                               const NleTimelineModel::Clip &c)
{
    if (body.width() <= 4 || body.height() <= 4) { return; }
    const qreal fps = qMax(1.0, mModel->fps());
    p.save();
    p.setClipRect(body, Qt::IntersectClip);
    p.setPen(QPen(cAudioWave, 1));
    const double mid = body.center().y();
    const auto waveIt = mWaves.constFind(c.clipId);
    const bool hasWave = waveIt != mWaves.constEnd() && !waveIt.value().isEmpty();
    const int n = int(body.width());
    for (int i = 0; i <= n; ++i) {
        double amp = 0.;
        const int frame = xToFrame(int(body.left()) + i);
        if (hasWave) {
            // real peaks: column time -> second + bucket lookup
            const double t = frame / fps;
            const int sec = int(t);
            const auto peaks = waveIt.value().constFind(sec);
            if (peaks != waveIt.value().constEnd() && !peaks.value().isEmpty()) {
                const qreal frac = t - sec;
                const int bucket = qBound(
                            0, int(frac * peaks.value().size()),
                            peaks.value().size() - 1);
                amp = peaks.value().at(bucket);
            }
        } else {
            // deterministic pseudo waveform until the real one lands
            quint32 hsh = quint32(c.clipId * 2654435761u) ^ quint32(i * 40503u);
            hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
            amp = (hsh % 1000) / 1000.0;
            amp = 0.15 + 0.85 * amp * (0.55 + 0.45 * qSin(i * 0.05));
        }
        const double x = body.left() + i;
        p.drawLine(QPointF(x, mid - amp * body.height() / 2),
                   QPointF(x, mid + amp * body.height() / 2));
    }
    p.restore();
}

void NleTimelineView::drawClip(QPainter &p,
                               const NleTimelineModel::Clip &c,
                               const bool ghost)
{
    NleTimelineModel::Move m{c.clipId, c.trackId, c.start, c.duration};
    if (!ghost) {
        const auto it = mCandidates.constFind(c.clipId);
        if (it != mCandidates.constEnd()) { m = it.value(); }
    }
    const QRectF r = moveRect(m);
    if (r.right() < headerWidth() || r.left() > width()) { return; }

    const bool selected = mModel->isSelected(c.clipId) && !ghost;
    const bool hovered = (c.clipId == mHoverId) && !ghost;

    p.save();
    if (ghost) { p.setOpacity(0.35); }
    else {
        const auto t = mModel->track(m.trackId);
        // 停用片段（CapCut）：块压暗，恢复后原样
        if (mModel->isDisabled(c.clipId)) { p.setOpacity(0.40); }
        else if (t && t->muted) { p.setOpacity(0.45); }
    }
    // keep everything of the clip inside the content area
    p.setClipRect(QRectF(headerWidth(), 0, width() - headerWidth(), height()));

    QPainterPath path;
    path.addRoundedRect(r, 4, 4);

    const int nameBarH = 16;

    // embedded-audio flag: a video-family clip with fed peaks shows
    // the kdenlive A/V layout (thumbs top, waveform strip bottom)
    const auto waveIt = mWaves.constFind(c.clipId);
    const bool hasEmbeddedWave = !c.audio &&
            waveIt != mWaves.constEnd() && !waveIt.value().isEmpty();

    // friction-style typed layer blocks (no media source): text =
    // yellow, shapes & vector containers = blue, adjustment = white
    // with a dashed border (action layer). The type bar doubles as
    // the name bar; no thumbnail strip
    const auto clipBox = enve_cast<BoundingBox*>(c.layer.data());
    QColor typeBody;
    bool isAdjust = false;
    if (clipBox) {
        switch (clipBox->getBoxType()) {
        case eBoxType::text:
            typeBody = QColor(0xf5, 0xa6, 0x23); break;            // 黄
        case eBoxType::adjustmentLayer:
            typeBody = QColor(0xd8, 0xd8, 0xd8);                   // 白
            isAdjust = true;
            break;
        case eBoxType::vectorPath:
        case eBoxType::circle:
        case eBoxType::rectangle:
        case eBoxType::layer:
            typeBody = QColor(0x2f, 0x6f, 0xd8); break;            // 蓝
        default:
            break;
        }
    }
    const bool typed = typeBody.isValid();

    if (typed) {
        p.fillPath(path, typeBody);
        if (isAdjust) {
            // action layer: dashed border keeps it visually distinct
            // from content layers even in white
            p.setPen(QPen(QColor(0x70, 0x70, 0x70), 1, Qt::DashLine));
            p.drawPath(path);
        }
        p.fillRect(QRectF(r.left(), r.top(), r.width(), nameBarH),
                   typeBody.darker(140));
        p.setPen(isAdjust ? QColor(0x33, 0x33, 0x33)
                          : QColor(0xff, 0xff, 0xff));
        QFont tf = font();
        tf.setPixelSize(10);
        p.setFont(tf);
        p.drawText(r.adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(
                       c.name, Qt::ElideRight, int(r.width() - 8)));
    } else if (!c.audio) {
        // body
        p.fillPath(path, QColor(0x2a, 0x2a, 0x2c));
        // thumbnail filmstrip below the name bar
        QRectF body(r.left(), r.top() + nameBarH, r.width(),
                    r.height() - nameBarH);
        QRectF waveBody;
        if (hasEmbeddedWave && body.height() > 16) {
            // kdenlive A/V clip: thumbs on top, embedded audio below
            waveBody = body;
            waveBody.setTop(body.top() + body.height() * 0.68);
            body.setHeight(body.height() * 0.68);
        }
        if (body.height() > 4) {
            p.save();
            p.setClipRect(body, Qt::IntersectClip);
            // real filmstrip first: decoded frames pinned to their own
            // time position (frame -> x directly)
            const auto stripIt = mFilm.constFind(c.clipId);
            const bool hasFilm = stripIt != mFilm.constEnd() && !stripIt.value().isEmpty();
            if (hasFilm) {
                for (auto it = stripIt.value().constBegin();
                     it != stripIt.value().constEnd(); ++it) {
                    const double x = frameToX(it.key());
                    const int w = qMax(2, int(it.value().width()
                                       * body.height() / it.value().height()));
                    if (x + w < body.left() || x > body.right()) { continue; }
                    p.drawImage(QRectF(x, body.top(), w, body.height()),
                                it.value());
                    p.setPen(QColor(0, 0, 0, 120));
                    p.drawLine(QPointF(x, body.top()), QPointF(x, body.bottom()));
                }
            } else {
                const QPixmap tile = thumbnailTile(
                            c.clipId, c.clipId * 37, int(body.height()));
                for (double x = body.left(); x < body.right(); x += tile.width()) {
                    p.drawPixmap(QPointF(x, body.top()), tile);
                    p.setPen(QColor(0, 0, 0, 120));
                    p.drawLine(QPointF(x, body.top()), QPointF(x, body.bottom()));
                }
            }
            p.restore();
        }
        if (waveBody.height() > 4) {
            p.fillRect(waveBody, QColor(0x16, 0x1d, 0x2c));
            drawWave(p, waveBody.adjusted(2, 1, -2, -1), c);
        }
        // name bar
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
        p.fillRect(QRectF(r.left(), r.top(), r.width(), nameBarH),
                   ghost ? cVideoBar.darker(160) : cVideoBar);
        p.restore();
    } else {
        // audio: dark blue body + waveform
        p.fillPath(path, cAudioBody);
        QRectF body = r.adjusted(2, nameBarH + 2, -2, -3);
        drawWave(p, body, c);
        drawVolumeEnvelope(p, c, body);
        p.save();
        p.setClipRect(r, Qt::IntersectClip);
        p.fillRect(QRectF(r.left(), r.top(), r.width(), nameBarH), cAudioBody.lighter(135));
        p.restore();
    }

    // clip name
    p.setPen(QColor(0xec, 0xec, 0xec));
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    if (!isAdjust) {
        p.drawText(r.adjusted(5, 0, -4, -(r.height() - nameBarH)),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(c.name, Qt::ElideRight, int(r.width() - 8)));
    }

    // speed badge (kdenlive shows the rate on sped-up clips): right
    // end of the name bar, e.g. "2x" / "0.5x"
    if (qAbs(c.speed - 1.) > 0.001) {
        const QString tag = QStringLiteral("%1x")
                .arg(QString::number(c.speed, 'g', 3));
        const QRectF tagRect(r.right() - 44, r.top() + 1, 42, nameBarH - 2);
        p.fillRect(tagRect, QColor(0, 0, 0, 110));
        p.setPen(QColor(0xff, 0xd1, 0x54));
        p.drawText(tagRect, Qt::AlignCenter, tag);
    }

    // CapCut audio info tag: source-media bitrate at the name-bar
    // right end (left of the speed badge when both show)
    if (c.audio && !ghost) {
        const QString tag = audioTagFor(c);
        if (!tag.isEmpty()) {
            const qreal off = qAbs(c.speed - 1.) > 0.001 ? 46. : 4.;
            const QRectF tagRect(r.right() - off - 52, r.top() + 1,
                                 50, nameBarH - 2);
            p.fillRect(tagRect, QColor(0, 0, 0, 110));
            p.setPen(QColor(0xb8, 0xd4, 0xf0));
            p.drawText(tagRect, Qt::AlignCenter, tag);
        }
    }

    // CapCut 颜色标记：名条右端小圆点（速度徽章左侧）
    if (!ghost) {
        const int mark = mModel->colorMark(c.clipId);
        if (mark >= 0) {
            static const QColor kMarkColors[7] = {
                QColor(0xe5, 0x48, 0x4d), QColor(0xf5, 0xa6, 0x23),
                QColor(0x46, 0xa7, 0x58), QColor(0x00, 0xb8, 0xa9),
                QColor(0x00, 0x90, 0xff), QColor(0xe9, 0x3d, 0x82),
                QColor(0x8e, 0x4e, 0xc6)};
            p.setRenderHint(QPainter::Antialiasing);
            const QPointF cc(r.right() - (qAbs(c.speed - 1.) > 0.001
                                              ? 50 : 9),
                             r.top() + nameBarH / 2.);
            p.setPen(QPen(QColor(0, 0, 0, 90), 1));
            p.setBrush(kMarkColors[qBound(0, mark, 6)]);
            p.drawEllipse(cc, 3.4, 3.4);
        }
    }

    // border: selected = white (CapCut), hovered = lighter
    p.setBrush(Qt::NoBrush); // drawPath would otherwise fill with the leftover badge brush
    if (selected) {
        p.setPen(QPen(Qt::white, 2));
        p.drawPath(path);
        // trim handles
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawRoundedRect(QRectF(r.left(), r.top(), 5, r.height()), 2, 2);
        p.drawRoundedRect(QRectF(r.right() - 5, r.top(), 5, r.height()), 2, 2);
    } else {
        p.setPen(QPen(hovered ? QColor(0x9a, 0x9a, 0x9a) : QColor(0x10, 0x10, 0x10), 1));
        p.drawPath(path);
    }

    // illegal drop feedback: red border + veil while the move overlaps
    if (mDropIllegal && c.clipId == mDragClipId && !ghost) {
        p.setPen(QPen(cPlayhead, 2));
        p.setBrush(QColor(cPlayhead.red(), cPlayhead.green(),
                          cPlayhead.blue(), 40));
        p.drawPath(path);
    }
    p.restore();
}

void NleTimelineView::drawPlayhead(QPainter &p)
{
    const int x = frameToX(mPlayheadFrame);
    if (x < headerWidth()) { return; }
    p.setPen(QPen(cPlayhead, 1.5));
    p.drawLine(x, 0, x, height());

    // handle in ruler
    QPolygonF head;
    head << QPointF(x - 7, 0) << QPointF(x + 7, 0)
         << QPointF(x + 7, 10) << QPointF(x, 18)
         << QPointF(x - 7, 10);
    p.setPen(Qt::NoPen);
    p.setBrush(cPlayhead);
    p.drawPolygon(head);
}

void NleTimelineView::refreshThemeColors()
{
    // follow the app theme: accent drives selection/highlight, its
    // darker shade the video clip name bars
    const QColor accent = ThemeSupport::getThemeHighlightColor();
    const QColor accentDark = ThemeSupport::getThemeHighlightDarkerColor();
    if (cAccent != accent) { cAccent = accent; }
    if (cVideoBar != accentDark) { cVideoBar = accentDark; }
}

// 媒体缓存按活 clipId 剪枝：块删除/工程重载后，其胶片条、波形、
// 中点缩略图与占位贴图不再被引用（clipId 不复用，留着就是纯泄漏）
void NleTimelineView::pruneMediaCaches()
{
    QSet<int> live;
    for (const auto &c : mModel->clips()) { live.insert(c.clipId); }

    auto pruneIntKey = [live](auto &map) {
        for (auto it = map.begin(); it != map.end();) {
            if (!live.contains(it.key())) { it = map.erase(it); }
            else { ++it; }
        }
    };
    pruneIntKey(mRealThumbs);
    pruneIntKey(mRealScaled);
    pruneIntKey(mFilm);
    pruneIntKey(mWaves);
    pruneIntKey(mAudioTags);

    // 占位贴图键 = "clipId x 高度"
    for (auto it = mThumbCache.begin(); it != mThumbCache.end();) {
        const QString key = it.key();
        const int xPos = key.indexOf(QLatin1Char('x'));
        const bool stale = xPos <= 0 ||
                !live.contains(key.left(xPos).toInt());
        if (stale) { it = mThumbCache.erase(it); }
        else { ++it; }
    }
}

// ---------------------------------------------------------------- picking

int NleTimelineView::clipAt(const QPoint &pos, QRectF *rectOut) const
{
    // topmost track last drawn wins: iterate from end
    const auto &clips = mModel->clips();
    for (int i = clips.size() - 1; i >= 0; --i) {
        const QRectF r = clipRect(clips.at(i));
        if (r.contains(pos)) {
            if (rectOut) { *rectOut = r; }
            return clips.at(i).clipId;
        }
    }
    return -1;
}

int NleTimelineView::snapFrame(const int frame,
                               const QSet<int> &ignoreIds,
                               bool *snappedOut) const
{
    int best = frame;
    double bestDist = SNAP_PX / mPxPerFrame;
    bool snapped = false;

    const auto consider = [&](const int cand) {
        const int d = qAbs(cand - frame);
        if (d < bestDist) { bestDist = d; best = cand; snapped = true; }
    };
    consider(mPlayheadFrame);
    for (const auto &c : mModel->clips()) {
        if (ignoreIds.contains(c.clipId)) { continue; }
        const auto m = effectiveMove(c.clipId);
        consider(m.start);
        consider(m.start + m.duration);
    }
    if (snappedOut) { *snappedOut = snapped; }
    return best;
}

// clips outside the moving set the dragged clip + its group riders
// would collide with; the tangled exemption (press-time overlap with
// the moving set) lets legacy overlap layouts untangle
bool NleTimelineView::dropLegal(const int primaryId,
                                const int targetTrackId,
                                const int newStart) const
{
    const auto pm = effectiveMove(primaryId);
    if (mModel->overlapOutside(targetTrackId, newStart, pm.duration,
                               mMovingIds, mTangled)) {
        return false;
    }
    // group riders shift by the same delta on their own lanes
    for (const int id : mMovingIds) {
        if (id == primaryId) { continue; }
        const auto m = effectiveMove(id);
        if (mModel->overlapOutside(m.trackId, m.start, m.duration,
                                   mMovingIds, mTangled)) {
            return false;
        }
    }
    return true;
}

// magnetic follow (CapCut): every same-track clip that started at or
// after the old out point shifts by the same amount the out point
// moved - shortening slides the chain left onto the new out point (no
// gap), lengthening pushes it right (no overlap). A clip STRADDLING
// the old out point (legacy overlap layouts) parks itself at the new
// out point instead of shifting by delta. Computed from the pristine
// model state every move, so it is idempotent/reversible.
QVector<NleTimelineModel::Move> NleTimelineView::magneticFollowMoves(
        const int clipId, const int oldEnd, const int newEnd) const
{
    QVector<NleTimelineModel::Move> moves;
    const int shift = newEnd - oldEnd;
    if (shift == 0) { return moves; }
    const auto c = mModel->clip(clipId);
    if (!c) { return moves; }
    for (const auto &o : mModel->clips()) {
        if (o.clipId == clipId || o.trackId != c->trackId) { continue; }
        if (o.start >= oldEnd) {
            moves.append({o.clipId, o.trackId,
                          qMax(0, o.start + shift), o.duration});
        } else if (o.start + o.duration > oldEnd) {
            // straddler: its head already sits inside the trimmed
            // clip - the tight chain wants it at the new out point
            moves.append({o.clipId, o.trackId,
                          qMax(0, newEnd), o.duration});
        }
    }
    return moves;
}

// ---------------------------------------------------------------- events

void NleTimelineView::setTool(const EditTool tool)
{
    if (mTool == tool) { return; }
    mTool = tool;
    // a started gesture keeps running under the tool it began with
    // (only the hover cursor changes; the release finishes normally)
    mRubber = false;
    applyToolCursor(mapFromGlobal(QCursor::pos()));
    update();
    emit toolChanged(static_cast<int>(tool));
}

void NleTimelineView::applyToolCursor(const QPoint &pos)
{
    // header area interactions come first: +V/+A buttons, lane height
    // resize edge, plain header
    if (pos.x() < headerWidth()) {
        if (pos.y() <= rulerHeight()) {
            setCursor((addTrackRect(false).contains(pos) ||
                       addTrackRect(true).contains(pos))
                          ? Qt::PointingHandCursor : Qt::ArrowCursor);
            return;
        }
        const int tr = trackAtY(pos.y());
        if (tr >= 0 && qAbs(pos.y() - (trackY(tr) + trackHeight(tr))) <= 4) {
            setCursor(Qt::SplitVCursor);
            return;
        }
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (mTool == EditTool::Razor) {
        setCursor(mRazorCursor);
        return;
    }
    if (mTool == EditTool::Hand) {
        setCursor(pos.y() > rulerHeight() ? Qt::OpenHandCursor
                                          : Qt::ArrowCursor);
        return;
    }
    if (mTool == EditTool::Spacer) {
        setCursor(pos.x() >= headerWidth() && pos.y() > rulerHeight()
                      ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    if (mTool != EditTool::Select) {
        // track-select tools point at the clips they will collect
        setCursor(clipAt(pos) >= 0 ? Qt::PointingHandCursor
                                   : Qt::ArrowCursor);
        return;
    }
    // select tool: volume envelope / trim edges / ruler hand / default
    QRectF r;
    const int clipId = clipAt(pos, &r);
    if (clipId >= 0 && volumeHitTest(pos, nullptr, nullptr) != VolHit::None) {
        // the envelope beats the trim edges (its hit zone is centered
        // in the body, the edges hug the clip sides)
        setCursor(Qt::SizeVerCursor);
    } else if (clipId >= 0 &&
        (qAbs(pos.x() - r.left()) <= TRIM_PX || qAbs(pos.x() - r.right()) <= TRIM_PX)) {
        setCursor(Qt::SizeHorCursor);
    } else if (pos.y() <= rulerHeight()) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
}

void NleTimelineView::setPlayheadFrame(const int frame)
{
    const int f = qMax(0, frame);
    if (f == mPlayheadFrame) { return; }
    const int oldX = frameToX(mPlayheadFrame);
    const int newX = frameToX(f);
    mPlayheadFrame = f;
    // 播放头逐帧移动只刷新旧两条竖带（标尺手柄 x±7 + 线宽 + 抗锯齿
    // 余量），播放/拖拽时不再每次整幅重绘全部块与波形
    update(QRect(oldX - 8, 0, 17, height())
               .united(QRect(newX - 8, 0, 17, height())));
}

// razor cut at the cursor position: one clip for a plain click, every
// unlocked-lane clip under the time column for Shift (PR: shift-razor
// cuts all tracks)
void NleTimelineView::razorCutAt(const QPoint &pos, const int clipId,
                                 const bool allTracks)
{
    const auto c = mModel->clip(clipId);
    if (!c) {
        // 落空也要留痕：否则日志无法区分“点了没切”与“点了没点上”
        qInfo("[NLE] razor miss frame=%d", xToFrame(pos.x()));
        return;
    }
    if (mModel->trackLocked(c->trackId)) {
        qInfo("[NLE] razor blocked track=%d frame=%d", c->trackId,
              xToFrame(pos.x()));
        emit logMessage(QStringLiteral("轨道已锁定，剪刀无效"));
        return;
    }
    const int frame = xToFrame(pos.x());
    QSet<int> ids;
    for (const auto &o : mModel->clips()) {
        if (allTracks) {
            if (mModel->trackLocked(o.trackId)) { continue; }
            if (!(o.start < frame && frame < o.start + o.duration)) { continue; }
        } else if (o.clipId != clipId) { continue; }
        ids.insert(o.clipId);
    }
    if (ids.isEmpty()) { return; }
    // selection-neutral: drop the selection so the refresh after the
    // cut does not resurrect both halves by name
    mModel->clearSelection();
    mModel->requestRazorCut(ids, frame);
    QList<int> sorted = ids.values();
    std::sort(sorted.begin(), sorted.end());
    QString idStr;
    for (const int id : sorted) {
        idStr += (idStr.isEmpty() ? QString() : QStringLiteral(","))
                 + QString::number(id);
    }
    qInfo("[NLE] razor cut frame=%d all=%d ids=%s", frame, int(allTracks),
          qUtf8Printable(idStr));
}

// PR track select: click collects the clicked clip plus every clip on
// the track in the tool direction from the click time; Shift widens
// to every track, Ctrl adds to the existing selection
void NleTimelineView::trackSelectAt(const QPoint &pos, const int clipId,
                                    const bool backward)
{
    const auto c = mModel->clip(clipId);
    if (!c) { return; }
    const int div = xToFrame(pos.x());
    const bool allTracks = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    QSet<int> picked;
    for (const auto &o : mModel->clips()) {
        const bool inDir = backward ? (o.start <= div)
                                    : (o.start + o.duration > div);
        if (!inDir) { continue; }
        if (!allTracks && o.trackId != c->trackId) { continue; }
        picked.insert(o.clipId);
    }
    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
        mModel->addToSelection(picked);
    } else {
        mModel->setSelection(picked);
    }
    const auto tr = mModel->track(c->trackId);
    emit logMessage(QStringLiteral("%1 %2 块（%3）")
            .arg(backward ? QStringLiteral("向左选择") : QStringLiteral("向右选择"))
            .arg(picked.size())
            .arg(allTracks ? QStringLiteral("全部轨道")
                           : (tr ? tr->name : QString())));
}

void NleTimelineView::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    // take the app keyboard routing with the click (kdenlive
    // tracksArea.focus): timeline keys stop falling through to the
    // canvas window from this click on
    KFT_setFocus();
    mPressPos = e->pos();

    // heal a drag whose release was lost (dock hidden mid-drag,
    // alt-tab with button held, a grab broken by the compositor, a
    // context menu swallowing the release): a fresh press always
    // starts clean. mRubber is part of the check on purpose - a
    // stuck rubber band eats EVERY move event (the early return in
    // mouseMoveEvent) while press-side selection keeps working, so
    // the timeline looks alive but no drag ever runs again
    if (mDrag != DragMode::None || mPending != DragMode::None || mRubber) {
        if (mDrag != DragMode::None || mRubber) {
            qInfo("[NLE] press heals stuck gesture drag=%d rubber=%d",
                  int(mDrag), int(mRubber));
        }
        mDrag = DragMode::None;
        mPending = DragMode::None;
        mPendingRoll = false;
        mRubber = false;
        mDragClipId = -1;
        mDragTrackIdx = -1;
        mSnapTarget = -1;
        mCandidates.clear();
        mMovingIds.clear();
        mTangled.clear();
        mSpacerOrig.clear();
        mDropIllegal = false;
        mGhostLane = false;
        mGhostLaneTop = false;
        mVolKey = nullptr;
        mVolAnim = nullptr;
        mVolClipId = -1;
        mModel->setGestureActive(false);
    }

    // hand pan: middle button anywhere, the Hand tool on the content
    // area (header badges keep their own interactions)
    if (e->button() == Qt::MiddleButton ||
            (mTool == EditTool::Hand && e->pos().x() >= headerWidth()
             && e->pos().y() > rulerHeight())) {
        mDrag = DragMode::Pan;
        mPanScroll = mScrollFrame;
        if (mTool == EditTool::Hand && e->button() == Qt::LeftButton) {
            setCursor(Qt::ClosedHandCursor);
        }
        return;
    }
    if (e->button() != Qt::LeftButton) { return; }
    // the hand tool in the header/ruler corners: nothing to pan
    if (mTool == EditTool::Hand) { return; }

    // clips win over the playhead: a press on a block must never start
    // a playhead drag, even when the block sits under the playhead line
    QRectF r;
    const int clipId = clipAt(e->pos(), &r);

    // corner square (+V / +A track buttons)
    if (e->pos().x() < headerWidth() && e->pos().y() <= rulerHeight()) {
        if (addTrackRect(false).contains(e->pos())) {
            mModel->requestTrackAdd(false, true);
        } else if (addTrackRect(true).contains(e->pos())) {
            mModel->requestTrackAdd(true);
        }
        return;
    }

    // ---- track header area: height resize / mute / lock ----
    if (e->pos().x() < headerWidth() && e->pos().y() > rulerHeight()) {
        const int tr = trackAtY(e->pos().y());
        if (tr >= 0 && tr < mModel->tracks().size()) {
            const int trackId = mModel->tracks().at(tr).id;
            // bottom edge of the header row = lane height resize
            if (qAbs(e->pos().y() - (trackY(tr) + trackHeight(tr))) <= 4) {
                mDrag = DragMode::TrackHeight;
                mDragTrackIdx = tr;
                mPressTrackHeight = trackHeight(tr);
                return;
            }
            if (muteBadgeRect(tr).contains(e->pos())) {
                // Shift = every track of the same type (kdenlive)
                mModel->requestTrackToggleMute(
                            trackId, e->modifiers() & Qt::ShiftModifier);
                return;
            }
            if (soloBadgeRect(tr).contains(e->pos())) {
                mModel->requestTrackToggleSolo(trackId);
                return;
            }
            if (lockBadgeRect(tr).contains(e->pos())) {
                mModel->requestTrackSetLocked(
                            trackId, !mModel->tracks().at(tr).locked);
                return;
            }
        }
        return; // plain header click: nothing (double-click renames)
    }

    // playhead: ruler area or near playhead line (when no clip is there)
    const int phx = frameToX(mPlayheadFrame);
    if (clipId < 0 && e->pos().x() >= headerWidth() &&
            (e->pos().y() <= rulerHeight() || qAbs(e->pos().x() - phx) <= 4)) {
        mDrag = DragMode::Playhead;
        setPlayheadFrame(xToFrame(e->pos().x()));
        emit playheadDragged(mPlayheadFrame);
        emit logMessage(QStringLiteral("播放头 -> %1").arg(timecode(mPlayheadFrame)));
        return;
    }

    if (mTool == EditTool::Spacer && e->pos().x() >= headerWidth()) {
        // spacer press works anywhere on a lane (clips or empty
        // space): everything right of the click joins one rigid run
        const int frame = xToFrame(e->pos().x());
        const int lane = trackAtY(e->pos().y());
        if (lane >= 0) {
            const bool allTracks = e->modifiers() & Qt::ShiftModifier;
            const int laneTrackId = mModel->tracks().value(lane).id;
            QSet<int> ids;
            mModel->clearSelection();
            mSpacerOrig.clear();
            for (const auto &c : mModel->clips()) {
                if (c.start + c.duration <= frame) { continue; }
                if (!allTracks && c.trackId != laneTrackId) { continue; }
                ids.insert(c.clipId);
                mSpacerOrig.append({c.clipId, c.start});
            }
            if (!mSpacerOrig.isEmpty()) {
                mModel->setSelection(ids);
                mMovingIds = ids;
                mTangled = mModel->collectTangleExemptions(ids);
                mCandidates.clear();
                // pending until the threshold: a plain click only
                // selects the run (kdenlive spacer)
                mPending = DragMode::SpacerMove;
                mPendingRoll = false;
                mDrag = DragMode::None;
                mDragClipId = -1;
                mDropIllegal = false;
                mSpacerPressFrame = frame;
                const auto tr = mModel->track(laneTrackId);
                emit logMessage(QStringLiteral("间隔工具：选中 %1 块（%2）")
                        .arg(mSpacerOrig.size())
                        .arg(allTracks ? QStringLiteral("全部轨道")
                                       : (tr ? tr->name : QString())));
            }
        }
        update();
        return;
    }

    // volume envelope beats the clip gestures (CapCut: pressing the
    // line/key never moves the clip)
    if (clipId >= 0 && mTool == EditTool::Select &&
            armVolumeGesture(e->pos(), e->modifiers() & Qt::AltModifier)) {
        return;
    }

    if (clipId >= 0 && mTool != EditTool::Select) {
        // editing tools replace the plain-click clip interaction
        if (mTool == EditTool::Razor) {
            razorCutAt(e->pos(), clipId, e->modifiers() & Qt::ShiftModifier);
        } else {
            trackSelectAt(e->pos(), clipId, mTool == EditTool::TrackBackward);
        }
        update();
        return;
    }

    if (clipId >= 0) {
        const auto c = mModel->clip(clipId);
        if (!c) { return; }
        // multi-select: ctrl toggles membership; a plain press on an
        // already-selected clip keeps the whole selection (group move)
        if (e->modifiers() & Qt::ControlModifier) {
            mModel->toggleInSelection(clipId);
        } else if (!mModel->isSelected(clipId)) {
            mModel->setSelection({clipId});
        }

        const bool laneLocked = mModel->trackLocked(c->trackId);
        mDragClipId = clipId;
        mDropIllegal = false;
        mCandidates.clear();
        // moving set: the selection at press time
        mMovingIds = mModel->selection();
        // press-time tangle snapshot: clips outside the moving set
        // that already overlap it (legacy overlap layouts) are exempt
        // from the drop legality check, so knots can still be dragged
        // apart
        mTangled = mModel->collectTangleExemptions(mMovingIds);

        if (laneLocked) {
            mPending = DragMode::None; // selectable but not editable
            mDragClipId = -1;
        } else {
            // pending until the drag threshold: a plain click only
            // selects (kdenlive MouseArea drag.threshold), a micro
            // jitter during the click must never rearrange a track
            const bool nearL = qAbs(e->pos().x() - r.left()) <= TRIM_PX;
            const bool nearR = qAbs(e->pos().x() - r.right()) <= TRIM_PX;
            mDrag = DragMode::None;
            mPendingRoll = (e->modifiers() & Qt::ControlModifier) &&
                    !mModel->magnetic();
            if (nearL && !nearR) {
                mPending = DragMode::TrimLeft;
            } else if (nearR) {
                mPending = DragMode::TrimRight;
            } else {
                mPending = DragMode::MoveClip;
                mGrabOffsetFrames = xToFrame(e->pos().x()) - c->start;
            }
        }
    } else if (e->pos().x() >= headerWidth()) {
        // empty content area
        if (!(e->modifiers() & Qt::ControlModifier)) {
            mModel->clearSelection();
        }
        // rubber band is a selection-tool gesture; with razor or track
        // tools a plain empty click just clears the selection
        if (mTool == EditTool::Select) {
            mRubber = true;
            mRubberStart = e->pos();
            mDrag = DragMode::None;
        }
    }
    update();
}

// promote the parked press gesture into a live drag once the pointer
// crossed the threshold; everything before that is click territory
void NleTimelineView::activatePendingDrag()
{
    mDrag = mPending;
    mPending = DragMode::None;
    mRoll = mPendingRoll;
    mPendingRoll = false;
    if (mDrag != DragMode::None) { mModel->setGestureActive(true); }
}

// live drag status feedback (kdenlive shows these as a drag tooltip):
// deduped so a frame-identical move does not re-light the status bar
void NleTimelineView::feedback(const QString &msg)
{
    if (msg == mLastFeedback) { return; }
    mLastFeedback = msg;
    emit logMessage(msg);
}

void NleTimelineView::mouseMoveEvent(QMouseEvent *e)
{
    // kdenlive drag.threshold: a pending gesture activates only after
    // the pointer moved a few pixels - until then the press stays a
    // click (select) and must not touch anything
    if (mPending != DragMode::None && mDrag == DragMode::None) {
        if ((e->pos() - mPressPos).manhattanLength() < DRAG_THRESHOLD_PX) {
            return;
        }
        activatePendingDrag();
    }

    if (mDrag == DragMode::None && !mRubber) {
        // hover + cursor feedback
        const int clipId = clipAt(e->pos());
        const bool hoverChanged = clipId != mHoverId;
        mHoverId = clipId;
        mHoverPos = e->pos();
        applyToolCursor(e->pos());
        if (hoverChanged || mTool == EditTool::Razor) { update(); }
        return;
    }

    // rubber band selection: live update, finalize on release
    if (mRubber) {
        update();
        return;
    }

    mSnapTarget = -1;
    const int curFrame = xToFrame(e->pos().x());

    switch (mDrag) {
    case DragMode::Playhead: {
        setPlayheadFrame(curFrame);
        emit playheadDragged(mPlayheadFrame);
        break;
    }
    case DragMode::Pan: {
        // hand pan: dragging left moves the content with the cursor
        const int df = qRound((mPressPos.x() - e->pos().x())
                              / mPxPerFrame);
        mScrollFrame = mPanScroll + df;
        clampView();
        updateScrollBar();
        break;
    }
    case DragMode::VolumePoint: {
        // vertical only (CapCut): the volume follows the cursor
        if (mVolKey && mVolClipId >= 0) {
            const auto c = mModel->clip(mVolClipId);
            if (c) {
                const QRectF body = clipRect(*c).adjusted(2, 18, -2, -3);
                const qreal t = 1. - qBound(
                            0., (e->pos().y() - body.top()) / body.height(),
                            1.);
                mVolKey->setValue(qRound(t * 200.));
                feedback(QStringLiteral("音量 %1%")
                             .arg(QString::number(t * 200., 'f', 0)));
            }
        }
        update();
        return; // envelope drags never auto-scroll the content
    }
    case DragMode::TrackHeight: {
        if (mDragTrackIdx >= 0 && mDragTrackIdx < mModel->tracks().size()) {
            const int dy = e->pos().y() - mPressPos.y();
            const int h = qBound(36, mPressTrackHeight + dy, 220);
            mHeightPreview.insert(mModel->tracks().at(mDragTrackIdx).id, h);
        }
        update();
        return; // header gestures never auto-scroll the content
    }
    case DragMode::SpacerMove: {
        int delta = curFrame - mSpacerPressFrame;
        // snap the run's leftmost edge like a normal drag would
        int minOrig = -1;
        for (const auto &s : mSpacerOrig) {
            minOrig = minOrig < 0 ? s.start : qMin(minOrig, s.start);
        }
        if (minOrig >= 0) {
            bool snapped = false;
            const int edge = qMax(0, minOrig + delta);
            const int sT = snapFrame(edge, {}, &snapped);
            if (snapped) {
                delta = sT - minOrig;
                mSnapTarget = sT;
            }
        }
        // rigid run legality: no collision outside the moving set
        bool legal = true;
        QHash<int, NleTimelineModel::Move> cand;
        for (const auto &s : mSpacerOrig) {
            const auto c = mModel->clip(s.clipId);
            if (!c) { continue; }
            const int ns = qMax(0, s.start + delta);
            if (mModel->overlapOutside(c->trackId, ns, c->duration,
                                       mMovingIds, mTangled)) {
                legal = false;
                break;
            }
            cand.insert(s.clipId, {s.clipId, c->trackId, ns, c->duration});
        }
        if (legal) {
            mDropIllegal = false;
            mCandidates = cand;
        } else {
            mDropIllegal = true;
            mSnapTarget = -1;
        }
        break;
    }
    case DragMode::MoveClip: {
        const auto c = mModel->clip(mDragClipId);
        if (!c) { break; }
        int newStart = qMax(0, curFrame - mGrabOffsetFrames);

        // CapCut lane lifecycle: dragging below every existing track
        // OR up into the ruler targets a NEW lane of the clip's type
        // (materializes on release - at the type group bottom or its
        // top, respectively); anywhere else keeps clamping to the
        // nearest same-type lane
        const int nTracks = mModel->tracks().size();
        const int lastBottom = nTracks
                ? trackY(nTracks - 1) + trackHeight(nTracks - 1)
                : rulerHeight();
        const bool newLaneBottom = e->pos().y() > lastBottom + 12;
        const bool newLaneTop = e->pos().y() < rulerHeight();
        const bool newLaneZone = newLaneBottom || newLaneTop;
        mGhostLane = newLaneZone;
        mGhostLaneTop = newLaneTop;
        mGhostLaneAudio = c->audio;
        int trIdx = newLaneZone ? -1 : trackAtY(e->pos().y());
        if (!newLaneZone &&
                (trIdx < 0 || mModel->tracks().value(trIdx).audio != c->audio)) {
            trIdx = nearestTrackOfType(e->pos().y(), c->audio);
        }
        const bool laneOk = newLaneZone ||
                (trIdx >= 0 && !mModel->tracks().value(trIdx).locked);
        const int trId = newLaneZone ? kGhostTrackId
                : (laneOk ? mModel->tracks().at(trIdx).id : c->trackId);

        // CapCut main-track magnetic: the rearrange applies to the
        // MAIN track alone (V1, the bottom video lane); drops onto
        // any other lane use the classic snap-and-refuse semantics
        if (mModel->magnetic() && trId != kGhostTrackId &&
                trId == mModel->mainTrackId()) {
            // kdenlive 方案A: the clip follows the mouse 1:1 on the
            // frame grid - no edge snapping, no refusals - and the
            // whole track rearranges around the drop live (left pack
            // compacts to 0, the rest chains tightly after)
            if (laneOk) {
                QHash<int, NleTimelineModel::Move> cand;
                for (const auto &m : mModel->magneticRearrangePlan(
                         trId, mDragClipId, newStart, mMovingIds)) {
                    cand.insert(m.clipId, m);
                }
                // riders on other tracks keep riding the time delta
                const int delta = newStart - c->start;
                for (const int id : mMovingIds) {
                    if (id == mDragClipId) { continue; }
                    const auto oc = mModel->clip(id);
                    if (!oc) { continue; }
                    cand.insert(id, {id, oc->trackId,
                                     qMax(0, oc->start + delta), oc->duration});
                }
                mCandidates = cand;
                mDropIllegal = false;
                feedback(QStringLiteral("偏移 %1%2 · 位置 %3").arg(
                             delta < 0 ? QStringLiteral("-")
                                       : QStringLiteral("+"),
                             timecode(qAbs(delta)),
                             timecode(newStart)));
            } else {
                mDropIllegal = true;
            }
            break;
        }

        bool snapped = false;
        const int sT = snapFrame(newStart, {mDragClipId}, &snapped);
        if (snapped) { newStart = sT; mSnapTarget = sT; }

        // overlap hard constraint: an illegal candidate keeps the last
        // legal state and flags the dragged clip red. Insert mode
        // (kdenlive toolbar toggle, Ctrl flips it for one gesture):
        // overlapping drops are allowed live and the release pushes
        // the run right instead
        const bool insertMode = mInsertMode ^
                bool(e->modifiers() & Qt::ControlModifier);
        QHash<int, NleTimelineModel::Move> cand = mCandidates;
        cand.insert(mDragClipId, {mDragClipId, trId, newStart, c->duration});
        // group move: every other selected clip rides the same time
        // delta (vertical lane changes stay single-clip on purpose)
        const int delta = newStart - c->start;
        for (const int id : mMovingIds) {
            if (id == mDragClipId) { continue; }
            const auto oc = mModel->clip(id);
            if (!oc) { continue; }
            cand.insert(id, {id, oc->trackId,
                             qMax(0, oc->start + delta), oc->duration});
        }
        if (laneOk && (insertMode || dropLegal(mDragClipId, trId, newStart))) {
            mCandidates = cand;
            mDropIllegal = false;
            feedback(QStringLiteral("%1 · 偏移 %2%3 · 位置 %4").arg(
                         insertMode ? QStringLiteral("插入模式")
                                    : QStringLiteral("覆盖模式"),
                         delta < 0 ? QStringLiteral("-")
                                   : QStringLiteral("+"),
                         timecode(qAbs(delta)),
                         timecode(newStart)));
        } else {
            mDropIllegal = true;
            mSnapTarget = -1;
        }
        break;
    }
    case DragMode::TrimLeft: {
        const auto c = mModel->clip(mDragClipId);
        if (!c) { break; }
        const int end = c->start + c->duration;
        const int minDur = mModel->minClipFrames();
        // kdenlive roll (Ctrl at press, non-magnetic): dragging the
        // shared cut drags BOTH sides - the previous clip's out point
        // follows this clip's in point, total length stays constant
        if (mRoll) {
            const NleTimelineModel::Clip *prev = nullptr;
            for (const auto &o : mModel->clips()) {
                if (o.clipId == c->clipId || o.trackId != c->trackId) { continue; }
                if (o.start + o.duration == c->start) { prev = &o; break; }
            }
            if (prev) {
                int ns = qBound(prev->start + minDur, curFrame,
                                end - minDur);
                mCandidates.insert(mDragClipId,
                                   {mDragClipId, c->trackId, ns, end - ns});
                mCandidates.insert(prev->clipId,
                                   {prev->clipId, prev->trackId,
                                    prev->start, ns - prev->start});
                feedback(QStringLiteral("卷动 · 左块出点 %1 · 右块入点 %2")
                             .arg(timecode(ns), timecode(ns)));
                break;
            }
        }
        int ns = qBound(0, curFrame, end - minDur);
        bool snapped = false;
        const int sT = snapFrame(ns, {mDragClipId}, &snapped);
        if (snapped) { ns = sT; mSnapTarget = sT; }
        // neighbor edge: the left handle cannot cross the previous
        // clip on the lane (tangled legacy overlaps are exempt so
        // the edge never gets pinned behind its own position)
        int lo = 0;
        int hi = 0;
        mModel->trimBounds(mDragClipId, mMovingIds, mTangled, &lo, &hi);
        lo = qMin(lo, end - minDur);
        ns = qMax(ns, lo);
        mCandidates.insert(mDragClipId,
                           {mDragClipId, c->trackId, ns, end - ns});
        feedback(QStringLiteral("入点 %1 · 时长 %2")
                     .arg(timecode(ns), timecode(end - ns)));
        break;
    }
    case DragMode::TrimRight: {
        const auto c = mModel->clip(mDragClipId);
        if (!c) { break; }
        const int minDur = mModel->minClipFrames();
        // kdenlive roll (Ctrl at press, non-magnetic): dragging the
        // shared cut drags BOTH sides - the next clip's in point
        // follows this clip's out point, total length stays constant
        if (mRoll) {
            const NleTimelineModel::Clip *next = nullptr;
            for (const auto &o : mModel->clips()) {
                if (o.clipId == c->clipId || o.trackId != c->trackId) { continue; }
                if (o.start == c->start + c->duration) { next = &o; break; }
            }
            if (next) {
                const int nextEnd = next->start + next->duration;
                int ne = qBound(c->start + minDur, curFrame,
                                nextEnd - minDur);
                mCandidates.insert(mDragClipId,
                                   {mDragClipId, c->trackId,
                                    c->start, ne - c->start});
                mCandidates.insert(next->clipId,
                                   {next->clipId, next->trackId,
                                    ne, nextEnd - ne});
                feedback(QStringLiteral("卷动 · 左块出点 %1 · 右块入点 %2")
                             .arg(timecode(ne), timecode(ne)));
                break;
            }
        }
        int ne = qMax(curFrame, c->start + minDur);
        bool snapped = false;
        const int sT = snapFrame(ne, {mDragClipId}, &snapped);
        if (snapped) { ne = sT; mSnapTarget = sT; }
        // magnetic follow: shortening slides attached neighbours left;
        // Alt keeps them in place for a plain trim (and a plain trim
        // respects the neighbor edge - the magnetic one pushes the
        // chain instead of clamping, that is the whole point of the
        // mode: extending a clip into its neighbour moves the
        // neighbour, it must not pin the edge at the current out)
        const bool follow = mModel->magnetic() &&
                c->trackId == mModel->mainTrackId() &&
                !(e->modifiers() & Qt::AltModifier);
        if (!follow) {
            // neighbor edge: the right handle cannot cross the next
            // clip's start on the lane (tangled legacy overlaps are
            // exempt, or the edge would be pinned BEHIND its own out
            // point and could only ever shrink)
            int lo = 0;
            int hi = INT_MAX;
            mModel->trimBounds(mDragClipId, mMovingIds, mTangled, &lo, &hi);
            hi = qMax(hi, c->start + minDur);
            ne = qMin(ne, hi);
        }
        mCandidates.insert(mDragClipId,
                           {mDragClipId, c->trackId, c->start, ne - c->start});
        const int oldEnd = c->start + c->duration;
        const auto followers = magneticFollowMoves(mDragClipId, oldEnd, ne);
        if (follow) {
            for (const auto &fm : followers) {
                mCandidates.insert(fm.clipId, fm);
            }
        } else {
            for (const auto &fm : followers) {
                const auto fo = mModel->clip(fm.clipId);
                if (fo) {
                    mCandidates.insert(fm.clipId,
                                       {fm.clipId, fm.trackId, fo->start, fo->duration});
                }
            }
        }
        feedback(QStringLiteral("出点 %1 · 时长 %2")
                     .arg(timecode(ne), timecode(ne - c->start)));
        break;
    }
    default: break;
    }

    // edge auto-scroll while dragging clips (never for the playhead or
    // header gestures)
    if (mDrag == DragMode::MoveClip || mDrag == DragMode::TrimLeft ||
            mDrag == DragMode::TrimRight || mDrag == DragMode::SpacerMove) {
        if (e->pos().x() > width() - 24) {
            mScrollFrame += qMax(1, qRound(20 / mPxPerFrame));
        } else if (e->pos().x() < headerWidth() + 24) {
            mScrollFrame = qMax(0, mScrollFrame - qMax(1, qRound(20 / mPxPerFrame)));
        }
        updateScrollBar();
    }
    update();
}

// end an editing gesture: flush the candidates into one model commit
// (insert-mode push-aside and magnetic compaction ride along), then
// reset the gesture state
void NleTimelineView::finishGestureCommit(const bool insertMode)
{
    QVector<NleTimelineModel::Move> moves;
    for (auto it = mCandidates.constBegin(); it != mCandidates.constEnd(); ++it) {
        moves.append(it.value());
    }
    // the insert-mode push-aside belongs to overwrite dragging; the
    // toolbar toggle and the Ctrl gesture-flip arrive here as one
    // flag; magnetic mode already rearranged the whole track live
    if (insertMode && !mModel->magnetic() &&
            mDrag == DragMode::MoveClip && mDragClipId >= 0) {
        const auto pm = mCandidates.value(mDragClipId);
        if (pm.clipId == mDragClipId) {
            const auto extra = mModel->insertShiftPlan(
                        pm.trackId, pm.start, pm.duration, mMovingIds);
            moves += extra;
            if (!extra.isEmpty()) {
                emit logMessage(QStringLiteral("插入模式：右侧整段右移"));
            }
        }
    }
    const bool hadMoves = !moves.isEmpty();
    mCandidates.clear();
    mMovingIds.clear();
    mTangled.clear();
    mSpacerOrig.clear();
    const DragMode drag = mDrag;
    qInfo("[NLE] gesture commit drag=%d moves=%d", int(drag), moves.size());
    mDrag = DragMode::None;
    mRoll = false;
    mDragClipId = -1;
    mSnapTarget = -1;
    mDropIllegal = false;
    mLastFeedback.clear();
    // releasing the gesture flushes queued rebuilds first (the doc is
    // still pristine - the gesture never wrote anything), then the
    // commit writes everything in one undoable pass. A magnetic move
    // commit IS the final layout (kdenlive 方案A: the dragged clip
    // lands at the mouse, the packs flow around it) - a follow-up
    // compaction would pull it back onto the left pack's tail
    mModel->setGestureActive(false);
    // CapCut lane lifecycle: a ghost-lane drop materializes the track
    // first (its refresh runs immediately - the gesture is over),
    // then the commit lands the clip on it and purges emptied lanes
    if (mGhostLane && hadMoves && drag == DragMode::MoveClip) {
        const int newId = mModel->requestTrackAdd(mGhostLaneAudio,
                                                  mGhostLaneTop);
        if (newId >= 0) {
            for (auto &mv : moves) {
                if (mv.trackId == kGhostTrackId) { mv.trackId = newId; }
            }
            emit logMessage(tr("已创建新轨道"));
        } else {
            moves.clear();
        }
    }
    mGhostLane = false;
    mGhostLaneTop = false;
    if (hadMoves && !moves.isEmpty()) {
        mModel->commitMoves(moves, drag != DragMode::MoveClip &&
                               mModel->magnetic());
    }
}

void NleTimelineView::mouseReleaseEvent(QMouseEvent *e)
{
    // pan may run on the middle button: finish it before the
    // left-button filter drops the release
    if (mDrag == DragMode::Pan &&
            (e->button() == Qt::MiddleButton ||
             e->button() == Qt::LeftButton)) {
        mDrag = DragMode::None;
        applyToolCursor(e->pos());
        update();
        return;
    }
    if (e->button() != Qt::LeftButton) { return; }

    // a press that never crossed the drag threshold: plain click, the
    // selection applied at press IS the whole edit (kdenlive
    // click-vs-drag semantics)
    if (mPending != DragMode::None) {
        mPending = DragMode::None;
        mPendingRoll = false;
        mSpacerOrig.clear();
        mMovingIds.clear();
        mTangled.clear();
        mCandidates.clear();
        update();
        return;
    }

    if (mRubber) {
        mRubber = false;
        const QRect band = QRect(mRubberStart, e->pos()).normalized();
        QSet<int> picked;
        for (const auto &c : mModel->clips()) {
            if (band.intersects(clipRect(c).toRect())) {
                picked.insert(c.clipId);
            }
        }
        if (!picked.isEmpty()) { mModel->addToSelection(picked); }
        update();
        return;
    }

    if (mDrag == DragMode::TrackHeight) {
        if (mDragTrackIdx >= 0 && mDragTrackIdx < mModel->tracks().size()) {
            const int trackId = mModel->tracks().at(mDragTrackIdx).id;
            mModel->requestTrackSetHeight(trackId, mHeightPreview.value(trackId));
        }
        mHeightPreview.clear();
        mDrag = DragMode::None;
        mDragTrackIdx = -1;
        update();
        return;
    }

    if (mDrag == DragMode::Playhead) {
        mDrag = DragMode::None;
        update();
        return;
    }

    if (mDrag == DragMode::VolumePoint) {
        mVolKey = nullptr;
        mVolAnim = nullptr;
        mVolClipId = -1;
        mDrag = DragMode::None;
        update();
        return;
    }

    if (mDrag != DragMode::None) {
        finishGestureCommit(mInsertMode ^
                            bool(e->modifiers() & Qt::ControlModifier));
        const auto sel = mModel->selection();
        if (sel.size() == 1) {
            const auto c = mModel->clip(*sel.constBegin());
            if (c) {
                emit logMessage(QStringLiteral("块“%1”：[%2]")
                        .arg(c->name, timecode(c->start)));
            }
        }
    }
    update();
}

void NleTimelineView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // double click track header name: rename the lane
    if (e->pos().x() < headerWidth() && e->pos().y() > rulerHeight()) {
        const int lane = trackAtY(e->pos().y());
        if (lane >= 0 && !muteBadgeRect(lane).contains(e->pos())
                && !lockBadgeRect(lane).contains(e->pos())
                && !soloBadgeRect(lane).contains(e->pos())) {
            renameTrackDialog(lane);
        }
        return;
    }
    // double click ruler: move playhead without drag
    if (e->pos().y() <= rulerHeight()) {
        setPlayheadFrame(xToFrame(e->pos().x()));
        emit playheadDragged(mPlayheadFrame);
        update();
    }
}

void NleTimelineView::wheelEvent(QWheelEvent *e)
{
    const qreal fps = qMax(1.0, mModel->fps());
    if (e->modifiers() & Qt::ControlModifier) {
        const double factor = e->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
        applyZoom(factor, e->position().x());
    } else {
        const double delta = -e->angleDelta().y() / 120.0; // steps
        // one wheel step scrolls ~40px worth of frames
        mScrollFrame = qMax(0, mScrollFrame +
                            qRound(delta * 40.0 / mPxPerFrame));
        clampView();
        updateScrollBar();
    }
    update();
    e->accept();
    Q_UNUSED(fps)
}

// ---------------------------------------------------------------- keys

void NleTimelineView::KFT_setFocusToWidget()
{
    setFocus();
    update();
}

// timeline keyboard主权 (kdenlive parity): every key handled here is
// consumed for the WHOLE app - returning true stops the KFT chain, so
// the canvas window never sees Delete again (it used to delete the
// stale canvas selection and steal the Qt focus back)
bool NleTimelineView::KFT_keyPressEvent(QKeyEvent *e)
{
    // MainWindow's Del special case re-dispatches ONE physical key
    // through two propagation chains (the eventFilter sendEvent lands
    // in MainWindow::keyPressEvent -> processKeyEvent -> KFT, then the
    // filter itself calls processKeyEvent again): the same event
    // object arrives twice with an identical timestamp - execute it
    // once (auto-repeat shares the timestamp too, so held-key repeats
    // stay swallowed as well)
    if (e->timestamp() == mLastKftTs && e->key() == mLastKftKey) {
        return true;
    }
    mLastKftTs = e->timestamp();
    mLastKftKey = e->key();

    const int key = e->key();
    const auto mods = e->modifiers();
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
        if (mModel->selection().isEmpty()) {
            emit logMessage(tr("无选中的块"));
        } else {
            requestDelete(mods & Qt::ShiftModifier);
        }
        return true;
    }
    if ((key == Qt::Key_C || key == Qt::Key_Slash) &&
            mods == Qt::NoModifier) {
        // no-selection press still splits: the model falls back to
        // every unlocked-lane clip under the playhead
        splitAtPlayhead();
        return true;
    }
    if (key == Qt::Key_A && mods == Qt::ControlModifier) {
        mModel->selectAll();
        return true;
    }
    // 时间轴块剪贴板（视图持有 KFT 焦点时接管，画布路由不参与）
    if (key == Qt::Key_C && mods == Qt::ControlModifier) {
        if (mModel->selection().isEmpty()) {
            emit logMessage(tr("无选中的块"));
        } else {
            mModel->requestCopy(mModel->selection());
        }
        return true;
    }
    if (key == Qt::Key_X && mods == Qt::ControlModifier) {
        if (mModel->selection().isEmpty()) {
            emit logMessage(tr("无选中的块"));
        } else {
            mModel->requestCopy(mModel->selection());
            requestDelete(false);
        }
        return true;
    }
    if (key == Qt::Key_V && mods == Qt::ControlModifier) {
        if (!mModel->hasClipClipboard()) {
            emit logMessage(tr("剪贴板没有块，先复制或剪切"));
        } else {
            mModel->requestPaste(mPlayheadFrame);
        }
        return true;
    }
    // CapCut 停用片段：播放时跳过（层隐藏 = 不渲染 + 静音）
    if (key == Qt::Key_E && mods == Qt::ShiftModifier) {
        if (mModel->selection().isEmpty()) {
            emit logMessage(tr("无选中的块"));
        } else {
            const bool disable =
                    !mModel->isDisabled(*mModel->selection().constBegin());
            mModel->requestSetDisabled(mModel->selection(), disable);
        }
        return true;
    }
    if (key == Qt::Key_Escape) {
        mModel->clearSelection();
        return true;
    }
    if (key == Qt::Key_Home && mods == Qt::NoModifier) {
        setPlayheadFrame(0);
        emit playheadDragged(0);
        return true;
    }
    if (key == Qt::Key_End && mods == Qt::NoModifier) {
        const int end = qMax(0, contentFrames() - 1);
        setPlayheadFrame(end);
        emit playheadDragged(end);
        return true;
    }
    // PR-style tool keys: V select, B razor, A track-select forward,
    // Shift+A track-select backward, D spacer
    if (key == Qt::Key_V && mods == Qt::NoModifier) {
        setTool(EditTool::Select);
        return true;
    }
    if (key == Qt::Key_B && mods == Qt::NoModifier) {
        setTool(EditTool::Razor);
        return true;
    }
    if (key == Qt::Key_A && mods == Qt::NoModifier) {
        setTool(EditTool::TrackForward);
        return true;
    }
    if (key == Qt::Key_A && mods & Qt::ShiftModifier) {
        setTool(EditTool::TrackBackward);
        return true;
    }
    if (key == Qt::Key_D && mods == Qt::NoModifier) {
        setTool(EditTool::Spacer);
        return true;
    }
    if (key == Qt::Key_H && mods == Qt::NoModifier) {
        setTool(EditTool::Hand);
        return true;
    }
    return false;
}

void NleTimelineView::leaveEvent(QEvent *)
{
    // a leave WHILE a button gesture runs means the implicit mouse
    // grab was broken (compositor took over, window lost the pointer):
    // the release will never arrive. Cancel the gesture right here -
    // a drag committed with candidates, a pending press and a rubber
    // band all reset, or the rubber branch would eat every future
    // move event while the press side keeps working (zombie timeline)
    if (mDrag != DragMode::None || mPending != DragMode::None || mRubber) {
        qInfo("[NLE] leave aborts gesture drag=%d pending=%d rubber=%d",
              int(mDrag), int(mPending), int(mRubber));
        if (mDrag == DragMode::TrackHeight) {
            mHeightPreview.clear();
            mDragTrackIdx = -1;
        } else if (mDrag != DragMode::None) {
            // drop outside = cancel: the candidates die, the document
            // was never touched during the gesture
            mCandidates.clear();
            mMovingIds.clear();
            mTangled.clear();
            mSpacerOrig.clear();
        }
        mDrag = DragMode::None;
        mPending = DragMode::None;
        mPendingRoll = false;
        mRubber = false;
        mDragClipId = -1;
        mSnapTarget = -1;
        mDropIllegal = false;
        mGhostLane = false;
        mGhostLaneTop = false;
        mLastFeedback.clear();
        mModel->setGestureActive(false);
    }
    mHoverId = -1;
    mHoverPos = QPoint(-1, -1);
    update();
}

void NleTimelineView::hideEvent(QHideEvent *)
{
    // dock toggled off: drop any drag state so reopening starts clean
    // (also unlocks queued rebuilds)
    mDrag = DragMode::None;
    mPending = DragMode::None;
    mPendingRoll = false;
    mRoll = false;
    mDragClipId = -1;
    mDragTrackIdx = -1;
    mSnapTarget = -1;
    mCandidates.clear();
    mMovingIds.clear();
    mTangled.clear();
    mSpacerOrig.clear();
    mDropIllegal = false;
    mGhostLane = false;
    mGhostLaneTop = false;
    mRubber = false;
    mModel->setGestureActive(false);
}

void NleTimelineView::resizeEvent(QResizeEvent *)
{
    updateScrollBar();
    emit viewChanged();
}

// ---------------------------------------------------------------- public ops

void NleTimelineView::splitAtPlayhead()
{
    mModel->requestSplitAtFrame(mPlayheadFrame);
}

void NleTimelineView::freezeAtPlayhead()
{
    const auto sel = mModel->selection();
    if (sel.isEmpty()) { return; }
    mModel->requestFreeze(sel, mPlayheadFrame);
}

void NleTimelineView::requestDelete(const bool ripple)
{
    const auto sel = mModel->selection();
    if (sel.isEmpty()) { return; }
    mModel->requestDelete(sel, ripple);
}

void NleTimelineView::applyZoom(const double factor, const int anchorX)
{
    const qreal fps = qMax(1.0, mModel->fps());
    const int anchorFrame = xToFrame(anchorX);
    mPxPerFrame = qBound(4.0 / fps, mPxPerFrame * factor, 1200.0 / fps);
    // keep the anchor frame under the cursor
    mScrollFrame = anchorFrame -
            qRound((anchorX - headerWidth()) / mPxPerFrame);
    clampView();
    updateScrollBar();
    emit viewChanged();
    update();
}

void NleTimelineView::zoomIn()
{
    applyZoom(1.3, headerWidth() + (width() - headerWidth()) / 2);
}

void NleTimelineView::zoomOut()
{
    applyZoom(1.0 / 1.3, headerWidth() + (width() - headerWidth()) / 2);
}

void NleTimelineView::zoomFit()
{
    const int dur = contentFrames();
    const int avail = width() - headerWidth();
    const qreal fps = qMax(1.0, mModel->fps());
    if (dur > 0 && avail > 0) {
        mPxPerFrame = qBound(4.0 / fps, double(avail) / dur, 1200.0 / fps);
    }
    mScrollFrame = 0;
    updateScrollBar();
    emit viewChanged();
    update();
}

void NleTimelineView::setZoomLevel(const int level)
{
    // toolbar zoom slider: 0-100 maps logarithmically over 10..600
    // px/sec, anchored on the playhead
    const qreal fps = qMax(1.0, mModel->fps());
    const double targetPps = 10.0 * std::pow(60.0, qBound(0, level, 100) / 100.0);
    const double target = targetPps / fps;
    if (qAbs(target - mPxPerFrame) < 0.001) { return; }
    const int anchor = qBound(headerWidth(), frameToX(mPlayheadFrame), width());
    applyZoom(target / mPxPerFrame, anchor);
}

void NleTimelineView::clampView()
{
    const int page = qRound((width() - headerWidth()) / mPxPerFrame);
    const int maxScroll = qMax(0, contentFrames() - page);
    const qreal fps = qMax(1.0, mModel->fps());
    mScrollFrame = qBound(0, mScrollFrame, maxScroll + qRound(2.0 * fps));
}

void NleTimelineView::updateScrollBar()
{
    clampView();
    if (!mScrollBar) { return; }
    const int page = qRound((width() - headerWidth()) / mPxPerFrame);
    const int content = contentFrames();
    const qreal fps = qMax(1.0, mModel->fps());
    mScrollBar->setRange(0, qMax(0, content - page));
    mScrollBar->setPageStep(page);
    mScrollBar->setValue(mScrollFrame);
    mScrollBar->setSingleStep(qMax(1, qRound(0.5 * fps)));
    emit viewChanged();
}

// ---------------------------------------------------------------- headers

QRect NleTimelineView::addTrackRect(const bool audio) const
{
    return QRect(audio ? 100 : 72, 6, 26, 18);
}

QRect NleTimelineView::muteBadgeRect(const int trackIdx) const
{
    const int top = trackY(trackIdx);
    const int h = trackHeight(trackIdx);
    return QRect(headerWidth() - 54, top + (h - 20) / 2, 20, 20);
}

QRect NleTimelineView::lockBadgeRect(const int trackIdx) const
{
    const int top = trackY(trackIdx);
    const int h = trackHeight(trackIdx);
    return QRect(headerWidth() - 82, top + (h - 20) / 2, 20, 20);
}

QRect NleTimelineView::soloBadgeRect(const int trackIdx) const
{
    const int top = trackY(trackIdx);
    const int h = trackHeight(trackIdx);
    return QRect(headerWidth() - 26, top + (h - 20) / 2, 20, 20);
}

// nearest lane of the requested type for a y position (free drags
// clamp here instead of creating implicit lanes)
int NleTimelineView::nearestTrackOfType(const int y, const bool audio) const
{
    int best = -1;
    int bestDist = INT_MAX;
    for (int i = 0; i < mModel->tracks().size(); ++i) {
        if (mModel->tracks().at(i).audio != audio) { continue; }
        const int mid = trackY(i) + trackHeight(i) / 2;
        const int d = qAbs(y - mid);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// shared by the header double-click and the context menu
void NleTimelineView::renameTrackDialog(const int trackIdx)
{
    if (trackIdx < 0 || trackIdx >= mModel->tracks().size()) { return; }
    const auto &track = mModel->tracks().at(trackIdx);
    bool ok = false;
    const QString name = QInputDialog::getText(
                this, tr("重命名轨道"), tr("轨道名称:"),
                QLineEdit::Normal, track.name, &ok);
    if (ok && !name.trimmed().isEmpty()) {
        mModel->requestTrackRename(track.id, name.trimmed());
    }
}

// ---------------------------------------------------------------- menus

// CapCut 右键菜单的标记颜色行：7 个色点 + 重置圈，点选即回收。
// 不用 Q_OBJECT（免 moc），点击通过回传出、菜单经 QPointer 关闭
class NleColorDotsRow : public QWidget
{
public:
    NleColorDotsRow(const int current, QWidget * const parent)
        : QWidget(parent)
    {
        setFixedSize(7 * 22 + 40, 26);
        setCursor(Qt::PointingHandCursor);
        if (current >= 0 && current < 7) { mCurrent = current; }
    }

    std::function<void(int)> picked;   // -1 = 清除
    QPointer<QMenu> menu;

private:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        static const QColor kColors[7] = {
            QColor(0xe5, 0x48, 0x4d), QColor(0xf5, 0xa6, 0x23),
            QColor(0x46, 0xa7, 0x58), QColor(0x00, 0xb8, 0xa9),
            QColor(0x00, 0x90, 0xff), QColor(0xe9, 0x3d, 0x82),
            QColor(0x8e, 0x4e, 0xc6)};
        const QColor dim = palette().color(QPalette::Disabled,
                                           QPalette::Text);
        for (int i = 0; i < 7; ++i) {
            const QPointF c(16 + i * 22, height() / 2.);
            if (mHover == i) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 28));
                p.drawEllipse(c, 9.5, 9.5);
            }
            p.setPen(QPen(QColor(0, 0, 0, 90), 1));
            p.setBrush(kColors[i]);
            p.drawEllipse(c, 6, 6);
        }
        // 重置圈（斜杠圆）：清除标记
        const QPointF c(16 + 7 * 22 + 6, height() / 2.);
        if (mHover == 7) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 28));
            p.drawEllipse(c, 9.5, 9.5);
        }
        QPen pen(dim);
        pen.setWidthF(1.4);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 5.5, 5.5);
        p.drawLine(QPointF(c.x() - 3.9, c.y() + 3.9),
                   QPointF(c.x() + 3.9, c.y() - 3.9));
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int h = hit(e->pos());
        if (h != mHover) { mHover = h; update(); }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const int h = hit(e->pos());
        if (h < 0) { return; }
        if (picked) { picked(h == 7 ? -1 : h); }
        if (menu) { menu->close(); }
    }

    void leaveEvent(QEvent *) override { mHover = -1; update(); }

    int hit(const QPoint &pos) const
    {
        const QPointF c(16 + 7 * 22 + 6, height() / 2.);
        const QPointF p(pos);
        if (std::hypot(p.x() - c.x(), p.y() - c.y()) <= 10) { return 7; }
        for (int i = 0; i < 7; ++i) {
            const QPointF cc(16 + i * 22, height() / 2.);
            if (std::hypot(p.x() - cc.x(), p.y() - cc.y()) <= 10) {
                return i;
            }
        }
        return -1;
    }

    int mHover = -1;
    int mCurrent = -1;
};

void NleTimelineView::contextMenuEvent(QContextMenuEvent *e)
{
    // ruler: marker management at the clicked frame
    if (e->pos().y() <= rulerHeight() && e->pos().x() >= headerWidth()) {
        const int frame = xToFrame(e->pos().x());
        QMenu menu(this);
        QAction *add = menu.addAction(tr("在此处添加标记"));
        int nearest = -1;
        int bestDist = qRound(12 * mPxPerFrame) + 8;
        for (const auto &mark : mMarkers) {
            const int d = qAbs(mark.first - frame);
            if (d < bestDist) { bestDist = d; nearest = mark.first; }
        }
        QAction *remove = menu.addAction(tr("移除最近的标记"));
        remove->setEnabled(nearest >= 0);
        QAction *act = menu.exec(e->globalPos());
        if (act == add) { mModel->requestMarkerAdd(frame); }
        else if (act == remove && nearest >= 0) {
            mModel->requestMarkerRemove(nearest);
        }
        return;
    }

    // track header: lifecycle menu (explicit tracks)
    if (e->pos().x() < headerWidth() && e->pos().y() > rulerHeight()) {
        const int lane = trackAtY(e->pos().y());
        if (lane < 0) { QWidget::contextMenuEvent(e); return; }
        const auto &track = mModel->tracks().at(lane);
        QMenu menu(this);
        // 加轨走角部 +V/+A 按钮，这里只留无快捷入口的生命周期操作
        QAction *del = menu.addAction(tr("删除轨道（仅空轨可删）"));
        QAction *rename = menu.addAction(tr("重命名轨道"));
        QAction *act = menu.exec(e->globalPos());
        if (act == del) { mModel->requestTrackRemove(track.id); }
        else if (act == rename) { renameTrackDialog(lane); }
        return;
    }

    const int clipId = clipAt(e->pos());
    if (clipId < 0) { QWidget::contextMenuEvent(e); return; }
    const auto c = mModel->clip(clipId);
    if (!c) { QWidget::contextMenuEvent(e); return; }
    if (!mModel->isSelected(clipId)) {
        mModel->setSelection({clipId});
    }
    const int srcIdx = mModel->trackIndex(c->trackId);
    const bool upOk = srcIdx > 0 &&
            mModel->tracks().at(srcIdx - 1).audio == c->audio;
    const bool downOk = srcIdx + 1 < mModel->tracks().size() &&
            mModel->tracks().at(srcIdx + 1).audio == c->audio;

    QMenu menu(this);
    // CapCut 分组：基础编辑（复制/剪切/删除）
    const bool anySel = !mModel->selection().isEmpty();
    QAction *copyAct = menu.addAction(tr("复制"));
    copyAct->setEnabled(anySel);
    copyAct->setShortcut(QKeySequence::Copy);
    QAction *cutAct = menu.addAction(tr("剪切"));
    cutAct->setEnabled(anySel);
    cutAct->setShortcut(QKeySequence::Cut);
    QAction *delAct = menu.addAction(tr("删除"));
    delAct->setEnabled(anySel);
    delAct->setShortcut(QKeySequence(Qt::Key_Backspace));
    menu.addSeparator();

    QAction *splitHere = menu.addAction(tr("在此处分割"));
    QAction *freeze = menu.addAction(tr("从此处定格到块尾"));
    QAction *speed = menu.addAction(tr("变速…"));
    // kdenlive detach-audio: only video-family clips with a live
    // embedded sound offer it
    const auto vidBox = enve_cast<VideoBox*>(c->layer.data());
    const auto embeddedSound = vidBox ? vidBox->sound() : nullptr;
    QAction *detach = menu.addAction(tr("分离音频"));
    detach->setEnabled(embeddedSound && embeddedSound->isVisible());
    menu.addSeparator();

    // CapCut 标记颜色行：7 色点 + 重置；作用于整个选中集
    auto *dots = new NleColorDotsRow(mModel->colorMark(clipId), &menu);
    dots->menu = &menu;
    dots->picked = [this](const int color) {
        const auto sel = mModel->selection();
        if (!sel.isEmpty()) { mModel->requestColorMark(sel, color); }
    };
    auto *dotsAct = new QWidgetAction(&menu);
    dotsAct->setDefaultWidget(dots);
    menu.addAction(dotsAct);
    QAction *sameColor = menu.addAction(tr("选中同色片段"));
    sameColor->setEnabled(mModel->colorMark(clipId) >= 0);
    QAction *disableAct = menu.addAction(tr("停用片段"));
    disableAct->setCheckable(true);
    disableAct->setChecked(mModel->isDisabled(clipId));
    disableAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_E));
    menu.addSeparator();

    QAction *up = nullptr;
    QAction *down = nullptr;
    if (upOk || downOk) {
        up = menu.addAction(tr("移动到上一轨"));
        up->setEnabled(upOk);
        down = menu.addAction(tr("移动到下一轨"));
        down->setEnabled(downOk);
    }

    QAction *act = menu.exec(e->globalPos());
    if (act == copyAct) {
        mModel->requestCopy(mModel->selection());
    } else if (act == cutAct) {
        mModel->requestCopy(mModel->selection());
        requestDelete(false);
    } else if (act == delAct) {
        requestDelete(false);
    } else if (act == splitHere) {
        mModel->clearSelection();
        mModel->requestRazorCut({clipId}, xToFrame(e->pos().x()));
    } else if (act == freeze) {
        // CapCut 定格: cut here + freeze everything from the cut to
        // the clip's end on the cut frame
        mModel->requestFreeze({clipId}, xToFrame(e->pos().x()));
    } else if (act == detach) {
        mModel->requestDetachAudio(clipId);
    } else if (act == sameColor) {
        mModel->requestSelectSameColor(clipId);
    } else if (act == disableAct) {
        mModel->requestSetDisabled(mModel->selection(),
                                   disableAct->isChecked());
    } else if (act == speed) {
        bool ok = false;
        const double rate = QInputDialog::getDouble(
                    this, tr("变速"), tr("播放速率（倍速，1=原速）"),
                    1.0, 0.1, 10.0, 2, &ok);
        if (ok && rate > 0.01) { mModel->requestSpeed(clipId, rate); }
    } else if (act == up && upOk) {
        mModel->requestMoveClipToTrack(
                    clipId, mModel->tracks().at(srcIdx - 1).id);
    } else if (act == down && downOk) {
        mModel->requestMoveClipToTrack(
                    clipId, mModel->tracks().at(srcIdx + 1).id);
    }
    update();
}

// ---------------------------------------------------------------- media

void NleTimelineView::setMarkers(const QVector<QPair<int, QString>> &markers)
{
    mMarkers = markers;
    update();
}

void NleTimelineView::setRangeBand(const int inFrame, const int outFrame)
{
    mRangeIn = inFrame;
    mRangeOut = outFrame;
    update();
}

void NleTimelineView::setClipThumbnail(const int clipId, const QImage &image)
{
    if (image.isNull()) { return; }
    mRealThumbs.insert(clipId, image);
    mRealScaled.remove(clipId);
    update();
}

void NleTimelineView::setClipThumbFrame(const int clipId, const int absFrame,
                                        const QImage &image)
{
    if (image.isNull()) { return; }
    mFilm[clipId].insert(absFrame, image);
    update();
}

void NleTimelineView::setClipWave(const int clipId, const int absSecond,
                                  const QVector<qreal> &peaks)
{
    if (peaks.isEmpty()) { return; }
    auto &wave = mWaves[clipId];
    if (wave.contains(absSecond) && wave.value(absSecond).size() == peaks.size()) {
        return; // unchanged: no repaint storm on every refresh
    }
    wave.insert(absSecond, peaks);
    update();
}

int NleTimelineView::viewStartFrame() const
{
    return xToFrame(headerWidth() + 1);
}

int NleTimelineView::viewEndFrame() const
{
    return xToFrame(width() - 1);
}
