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
#include <algorithm>
#include <climits>

#include "themesupport.h"

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
    // caches survive because clip ids are stable per layer
    connect(mModel, &NleTimelineModel::modelChanged,
            this, [this]() { updateScrollBar(); update(); });
    connect(mModel, &NleTimelineModel::selectionChanged,
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
    const int idx = mModel->trackIndex(m.trackId);
    const int top = trackY(idx);
    return QRectF(x, top + 2, w, trackHeight(idx) - 4);
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

void NleTimelineView::drawTrackHeaders(QPainter &p)
{
    const auto &tracks = mModel->tracks();
    for (int i = 0; i < tracks.size(); ++i) {
        const int top = trackY(i);
        const int h = trackHeight(i);
        p.fillRect(0, top, headerWidth(), h, cHeader);
        p.setPen(cGridLine);
        p.drawLine(0, top + h - 1, headerWidth(), top + h - 1);
        p.drawLine(headerWidth() - 1, top, headerWidth() - 1, top + h);

        // text-icon per track type
        const QString icon = tracks[i].audio ? QStringLiteral("A") : QStringLiteral("V");
        const QColor ic = tracks[i].audio ? cAudioWave : cAccent;
        const QRect badge(10, top + (h - 22) / 2, 22, 22);
        p.setPen(Qt::NoPen);
        p.setBrush(ic.darker(130));
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(QColor(0xe8, 0xe8, 0xe8));
        p.drawText(badge, Qt::AlignCenter, icon);

        p.setPen(tracks[i].muted ? cTextDim : cText);
        p.drawText(QRect(38, top, headerWidth() - 68, h),
                   Qt::AlignVCenter, tracks[i].name);

        // mute badge: video lane hides the picture, audio lane mutes
        // the sound (one visibility op, glyph says which)
        const QRect mb = muteBadgeRect(i);
        const bool laneMuted = tracks[i].muted;
        p.setPen(QPen(laneMuted ? QColor(0xe8, 0x4c, 0x4c) : cGridLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(mb, 4, 4);
        p.setPen(laneMuted ? QColor(0xe8, 0x4c, 0x4c) : cTextDim);
        p.drawText(mb, Qt::AlignCenter,
                   tracks[i].audio ? QStringLiteral("静") : QStringLiteral("隐"));

        // lock badge (L): dim when off, amber when the lane is locked
        const QRect lb = lockBadgeRect(i);
        p.setPen(QPen(tracks[i].locked ? QColor(0xff, 0xd1, 0x54) : cGridLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(lb, 4, 4);
        p.setPen(tracks[i].locked ? QColor(0xff, 0xd1, 0x54) : cTextDim);
        p.drawText(lb, Qt::AlignCenter, QStringLiteral("L"));
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
        if (t && t->muted) { p.setOpacity(0.45); }
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

    if (!c.audio) {
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
    p.drawText(r.adjusted(5, 0, -4, -(r.height() - nameBarH)),
               Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(c.name, Qt::ElideRight, int(r.width() - 8)));

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

    // border: selected = accent, hovered = lighter
    p.setBrush(Qt::NoBrush); // drawPath would otherwise fill with the leftover badge brush
    if (selected) {
        p.setPen(QPen(cAccent, 2));
        p.drawPath(path);
        // trim handles
        p.setPen(Qt::NoPen);
        p.setBrush(cAccent);
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
// gap), lengthening pushes it right (no overlap). Computed from the
// pristine model state every move, so it is idempotent/reversible.
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
    // select tool: trim edges / ruler hand / default
    QRectF r;
    const int clipId = clipAt(pos, &r);
    if (clipId >= 0 &&
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
    mPlayheadFrame = f;
    update();
}

// razor cut at the cursor position: one clip for a plain click, every
// unlocked-lane clip under the time column for Shift (PR: shift-razor
// cuts all tracks)
void NleTimelineView::razorCutAt(const QPoint &pos, const int clipId,
                                 const bool allTracks)
{
    const auto c = mModel->clip(clipId);
    if (!c) { return; }
    if (mModel->trackLocked(c->trackId)) {
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

// direction-only variant for the context menu: every clip on every
// track before/after the click time
void NleTimelineView::trackSelectAll(const QPoint &pos, const bool backward)
{
    const int div = xToFrame(pos.x());
    QSet<int> picked;
    for (const auto &o : mModel->clips()) {
        const bool inDir = backward ? (o.start <= div)
                                    : (o.start + o.duration > div);
        if (inDir) { picked.insert(o.clipId); }
    }
    mModel->setSelection(picked);
    emit logMessage(QStringLiteral("%1 %2 块（全部轨道）")
            .arg(backward ? QStringLiteral("向左选择") : QStringLiteral("向右选择"))
            .arg(picked.size()));
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
    // alt-tab with button held): a fresh press always starts clean
    if (mDrag != DragMode::None || mPending != DragMode::None) {
        mDrag = DragMode::None;
        mPending = DragMode::None;
        mPendingRoll = false;
        mDragClipId = -1;
        mDragTrackIdx = -1;
        mSnapTarget = -1;
        mCandidates.clear();
        mMovingIds.clear();
        mTangled.clear();
        mSpacerOrig.clear();
        mDropIllegal = false;
        mModel->setGestureActive(false);
    }

    if (e->button() == Qt::MiddleButton) {
        return; // reserved: pan view
    }
    if (e->button() != Qt::LeftButton) { return; }

    // clips win over the playhead: a press on a block must never start
    // a playhead drag, even when the block sits under the playhead line
    QRectF r;
    const int clipId = clipAt(e->pos(), &r);

    // corner square (+V / +A track buttons)
    if (e->pos().x() < headerWidth() && e->pos().y() <= rulerHeight()) {
        if (addTrackRect(false).contains(e->pos())) {
            mModel->requestTrackAdd(false);
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

        // lane under the cursor, clamped to the nearest same-type
        // lane: tracks are explicit entities, hovering outside never
        // spawns a throwaway lane
        int trIdx = trackAtY(e->pos().y());
        if (trIdx < 0 || mModel->tracks().value(trIdx).audio != c->audio) {
            trIdx = nearestTrackOfType(e->pos().y(), c->audio);
        }
        const bool laneOk = trIdx >= 0 && !mModel->tracks().value(trIdx).locked;
        const int trId = laneOk ? mModel->tracks().at(trIdx).id : c->trackId;

        if (mModel->magnetic()) {
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
        // clip on the lane
        int lo = 0;
        int hi = 0;
        mModel->trimBounds(mDragClipId, mMovingIds, &lo, &hi);
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
        // neighbor edge: the right handle cannot cross the next clip's
        // start on the lane
        int lo = 0;
        int hi = INT_MAX;
        mModel->trimBounds(mDragClipId, mMovingIds, &lo, &hi);
        hi = qMax(hi, c->start + minDur);
        ne = qMin(ne, hi);
        mCandidates.insert(mDragClipId,
                           {mDragClipId, c->trackId, c->start, ne - c->start});
        // magnetic follow: shortening the out point slides attached
        // neighbours left; Alt keeps them in place for a plain trim
        const bool follow = mModel->magnetic() &&
                !(e->modifiers() & Qt::AltModifier);
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
    if (hadMoves) {
        mModel->commitMoves(moves, drag != DragMode::MoveClip &&
                               mModel->magnetic());
    }
}

void NleTimelineView::mouseReleaseEvent(QMouseEvent *e)
{
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
                && !lockBadgeRect(lane).contains(e->pos())) {
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
    if ((key == Qt::Key_S || key == Qt::Key_Slash) &&
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
    return false;
}

void NleTimelineView::leaveEvent(QEvent *)
{
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
    return QRect(headerWidth() - 58, top + (h - 20) / 2, 20, 20);
}

QRect NleTimelineView::lockBadgeRect(const int trackIdx) const
{
    const int top = trackY(trackIdx);
    const int h = trackHeight(trackIdx);
    return QRect(headerWidth() - 32, top + (h - 20) / 2, 20, 20);
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
        QAction *add = menu.addAction(track.audio ? tr("添加音频轨")
                                                  : tr("添加视频轨"));
        QAction *del = menu.addAction(tr("删除轨道（仅空轨可删）"));
        QAction *rename = menu.addAction(tr("重命名轨道"));
        QAction *act = menu.exec(e->globalPos());
        if (act == add) { mModel->requestTrackAdd(track.audio); }
        else if (act == del) { mModel->requestTrackRemove(track.id); }
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
    // NLE editing section (always available)
    QAction *splitHere = menu.addAction(tr("在此处分割"));
    QAction *del = menu.addAction(tr("删除"));
    QAction *rippleDel = menu.addAction(tr("波纹删除"));
    QAction *freeze = menu.addAction(tr("从此处定格到块尾"));
    QAction *speed = menu.addAction(tr("变速…"));
    menu.addSeparator();
    QAction *back = menu.addAction(tr("向左选择本轨"));
    QAction *backAll = menu.addAction(tr("向左选择全部轨道"));
    QAction *fwd = menu.addAction(tr("向右选择本轨"));
    QAction *fwdAll = menu.addAction(tr("向右选择全部轨道"));
    QAction *up = nullptr;
    QAction *down = nullptr;
    if (upOk || downOk) {
        menu.addSeparator();
        up = menu.addAction(tr("移动到上一轨"));
        up->setEnabled(upOk);
        down = menu.addAction(tr("移动到下一轨"));
        down->setEnabled(downOk);
    }

    QAction *act = menu.exec(e->globalPos());
    if (act == splitHere) {
        mModel->clearSelection();
        mModel->requestRazorCut({clipId}, xToFrame(e->pos().x()));
    } else if (act == del) { requestDelete(false); }
    else if (act == rippleDel) { requestDelete(true); }
    else if (act == freeze) {
        // CapCut 定格: cut here + freeze everything from the cut to
        // the clip's end on the cut frame
        mModel->requestFreeze({clipId}, xToFrame(e->pos().x()));
    } else if (act == speed) {
        bool ok = false;
        const double rate = QInputDialog::getDouble(
                    this, tr("变速"), tr("播放速率（倍速，1=原速）"),
                    1.0, 0.1, 10.0, 2, &ok);
        if (ok && rate > 0.01) { mModel->requestSpeed(clipId, rate); }
    } else if (act == back) { trackSelectAt(e->pos(), clipId, true); }
    else if (act == backAll) { trackSelectAll(e->pos(), true); }
    else if (act == fwd) { trackSelectAt(e->pos(), clipId, false); }
    else if (act == fwdAll) { trackSelectAll(e->pos(), false); }
    else if (act == up && upOk) {
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
