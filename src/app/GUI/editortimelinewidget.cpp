#include "editortimelinewidget.h"

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
#include <QDateTime>
#include <QDebug>
#include <QCursor>
#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QSet>
#include <algorithm>
#include <climits>

#include "themesupport.h"

static const double MIN_CLIP_LEN = 0.2;   // seconds
static const int SNAP_PX = 8;
static const int TRIM_PX = 6;

EditorTimelineWidget::EditorTimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(320);

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
        m_razorCursor = QCursor(pm, 5, 19);
    }

    // content (tracks + clips) is filled by the sync-driven rebuild
    emitLog(QStringLiteral("timeline ready"));
}

void EditorTimelineWidget::setScrollBar(QScrollBar *bar)
{
    m_scrollBar = bar;
    if (!m_scrollBar) return;
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int v){
        m_scrollSec = v / 100.0;
        update();
    });
    updateScrollBar();
}

// ---------------------------------------------------------------- mapping

int EditorTimelineWidget::trackY(int track) const
{
    int y = rulerHeight();
    for (int i = 0; i < track && i < m_tracks.size(); ++i)
        y += m_tracks[i].height;
    return y;
}

int EditorTimelineWidget::trackAtY(int y) const
{
    if (y < rulerHeight()) return -1;
    for (int i = 0; i < m_tracks.size(); ++i) {
        int top = trackY(i);
        if (y >= top && y < top + m_tracks[i].height) return i;
    }
    return -1;
}

double EditorTimelineWidget::xToTime(int x) const
{
    return m_scrollSec + (x - headerWidth()) / m_pxPerSec;
}

int EditorTimelineWidget::timeToX(double t) const
{
    return headerWidth() + qRound((t - m_scrollSec) * m_pxPerSec);
}

double EditorTimelineWidget::contentDuration() const
{
    double end = 30.0;
    for (const Clip &c : m_clips)
        end = qMax(end, c.start + c.length + 5.0);
    return end;
}

QRectF EditorTimelineWidget::clipRect(const Clip &c) const
{
    double x = timeToX(c.start);
    double w = c.length * m_pxPerSec;
    int top = trackY(c.track);
    return QRectF(x, top + 2, w, m_tracks[c.track].height - 4);
}

// ---------------------------------------------------------------- painting

void EditorTimelineWidget::paintEvent(QPaintEvent *)
{
    refreshThemeColors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), cBg);

    drawTrackBodies(p);
    drawRuler(p);
    drawTrackHeaders(p);

    // clips
    for (int i = 0; i < m_clips.size(); ++i)
        drawClip(p, i);

    // ghost of dragged clip at original position
    if (m_drag == DragMode::MoveClip && m_dragClip >= 0 && m_dragClip < m_clips.size()) {
        const Clip &cur = m_clips[m_dragClip];
        if (qAbs(cur.start - m_origStart) > 1e-6 || cur.track != m_origTrack)
            drawClip(p, m_dragClip, true);
    }

    // snap guide line
    if (m_snapTarget >= 0.0) {
        int x = timeToX(m_snapTarget);
        p.setPen(QPen(QColor(0xff, 0xd1, 0x54), 1, Qt::DashLine));
        p.drawLine(x, rulerHeight(), x, height());
    }

    // rubber band selection overlay
    if (m_rubber) {
        const QRect band = QRect(m_rubberStart, mapFromGlobal(QCursor::pos()))
                .normalized().intersected(rect().adjusted(headerWidth(), rulerHeight(), -1, -1));
        if (!band.isEmpty()) {
            p.setPen(QPen(cAccent, 1, Qt::DashLine));
            p.setBrush(QColor(cAccent.red(), cAccent.green(), cAccent.blue(), 30));
            p.drawRect(band);
        }
    }

    // razor guide: dashed cut preview under the cursor across the
    // whole content area (matches the playhead red)
    if (m_tool == EditTool::Razor && m_drag == DragMode::None &&
            m_hoverPos.x() >= headerWidth() && m_hoverPos.y() > rulerHeight()) {
        const int rx = m_hoverPos.x();
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
        const bool hov = b.contains(m_hoverPos);
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

void EditorTimelineWidget::drawRuler(QPainter &p)
{
    p.fillRect(headerWidth(), 0, width() - headerWidth(), rulerHeight(), cRuler);

    // choose tick step so labels stay readable
    double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60, 300};
    double step = 1;
    for (double s : steps) { if (s * m_pxPerSec >= 70) { step = s; break; } }

    p.setFont(font());
    double t0 = qMax(0.0, m_scrollSec - step);
    double first = qFloor(t0 / step) * step;
    for (double t = first; ; t += step) {
        int x = timeToX(t);
        if (x > width() + 4) break;
        if (x < headerWidth()) continue;
        p.setPen(cGridLine);
        p.drawLine(x, rulerHeight() - 8, x, rulerHeight());
        p.setPen(cTextDim);
        p.drawText(QRect(x + 3, 2, 90, rulerHeight() - 10),
                   Qt::AlignLeft | Qt::AlignVCenter, timecode(t));
        // minor ticks
        for (int k = 1; k < 4; ++k) {
            int mx = timeToX(t + step * k / 4.0);
            if (mx <= width()) {
                p.setPen(QColor(0x26, 0x26, 0x26));
                p.drawLine(mx, rulerHeight() - 4, mx, rulerHeight());
            }
        }
    }

    // scene markers: amber guides through the whole timeline
    for (const auto &mark : m_markers) {
        const int x = timeToX(mark.first / m_fps);
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

void EditorTimelineWidget::drawTrackHeaders(QPainter &p)
{
    for (int i = 0; i < m_tracks.size(); ++i) {
        int top = trackY(i);
        p.fillRect(0, top, headerWidth(), m_tracks[i].height, cHeader);
        p.setPen(cGridLine);
        p.drawLine(0, top + m_tracks[i].height - 1, headerWidth(), top + m_tracks[i].height - 1);
        p.drawLine(headerWidth() - 1, top, headerWidth() - 1, top + m_tracks[i].height);

        // text-icon per track type
        QString icon = m_tracks[i].type == ClipType::Video ? QStringLiteral("V") : QStringLiteral("A");
        QColor ic = m_tracks[i].type == ClipType::Video ? cAccent : cAudioWave;
        QRect badge(10, top + (m_tracks[i].height - 22) / 2, 22, 22);
        p.setPen(Qt::NoPen);
        p.setBrush(ic.darker(130));
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(QColor(0xe8, 0xe8, 0xe8));
        p.drawText(badge, Qt::AlignCenter, icon);

        p.setPen(m_tracks[i].muted ? cTextDim : cText);
        p.drawText(QRect(38, top, headerWidth() - 68, m_tracks[i].height),
                   Qt::AlignVCenter, m_tracks[i].name);

        // mute badge: video lane hides the picture, audio lane mutes the
        // sound (one visibility op in friction, glyph says which)
        const QRect mb = muteBadgeRect(i);
        const bool laneMuted = m_tracks[i].muted;
        p.setPen(QPen(laneMuted ? QColor(0xe8, 0x4c, 0x4c) : cGridLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(mb, 4, 4);
        p.setPen(laneMuted ? QColor(0xe8, 0x4c, 0x4c) : cTextDim);
        p.drawText(mb, Qt::AlignCenter,
                   m_tracks[i].type == ClipType::Video
                       ? QStringLiteral("隐") : QStringLiteral("静"));

        // lock badge (L): dim when off, amber when the lane is locked
        const QRect lb = lockBadgeRect(i);
        p.setPen(QPen(m_tracks[i].locked ? QColor(0xff, 0xd1, 0x54) : cGridLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(lb, 4, 4);
        p.setPen(m_tracks[i].locked ? QColor(0xff, 0xd1, 0x54) : cTextDim);
        p.drawText(lb, Qt::AlignCenter, QStringLiteral("L"));
    }
}

void EditorTimelineWidget::drawTrackBodies(QPainter &p)
{
    for (int i = 0; i < m_tracks.size(); ++i) {
        int top = trackY(i);
        p.fillRect(headerWidth(), top, width() - headerWidth(), m_tracks[i].height,
                   (i % 2) ? cBgAlt : cBg);
        p.setPen(QColor(0x26, 0x26, 0x26));
        p.drawLine(headerWidth(), top + m_tracks[i].height - 1, width(), top + m_tracks[i].height - 1);
    }
}

QString EditorTimelineWidget::timecode(double t) const
{
    const int ffTot = qRound(t * m_fps);
    const int mm = ffTot / (60 * qMax(1, int(m_fps)));
    const int ss = (ffTot / qMax(1, int(m_fps))) % 60;
    const int ff = ffTot % qMax(1, int(m_fps));
    return QStringLiteral("%1:%2:%3")
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

QPixmap EditorTimelineWidget::thumbnailTile(const Clip &c, int h)
{
    // real rendered frame first: scaled to the strip height once, then
    // repeated across the clip body like the placeholder tiles
    const auto raw = m_realThumbs.constFind(c.id);
    if (raw != m_realThumbs.constEnd() && !raw.value().isNull()) {
        auto scaled = m_realScaled.constFind(c.id);
        if (scaled == m_realScaled.constEnd() || scaled.value().height() != h) {
            const QPixmap pm = QPixmap::fromImage(
                        raw.value().scaledToHeight(h, Qt::SmoothTransformation));
            if (scaled != m_realScaled.constEnd()) m_realScaled[c.id] = pm;
            else m_realScaled.insert(c.id, pm);
            return pm;
        }
        return scaled.value();
    }
    // one tile per clip; repeated across the clip body
    const int tileW = qMax(48, int(h * 16.0 / 9.0));
    QString key = QStringLiteral("%1x%2").arg(c.id).arg(h);
    auto it = m_thumbCache.find(key);
    if (it != m_thumbCache.end()) return it.value();

    QPixmap pm(tileW, h);
    pm.fill(Qt::transparent);
    QPainter tp(&pm);
    tp.setRenderHint(QPainter::Antialiasing);

    QRandomGenerator rng(quint32(c.hueSeed));
    // base gradient, hue varies per clip
    int hue = (c.hueSeed * 47) % 360;
    QColor base = QColor::fromHsv(hue, 110, 150);
    QColor base2 = QColor::fromHsv((hue + 40) % 360, 130, 110);
    QLinearGradient g(0, 0, tileW, h);
    g.setColorAt(0, base);
    g.setColorAt(1, base2);
    tp.fillRect(pm.rect(), g);

    // scenery-ish shapes so tiles look like frames
    for (int i = 0; i < 4; ++i) {
        int w = 10 + rng.bounded(tileW / 2);
        int hh = 6 + rng.bounded(h / 2);
        int x = rng.bounded(qMax(1, tileW - w));
        int y = rng.bounded(qMax(1, h - hh));
        QColor c2 = QColor::fromHsv((hue + rng.bounded(120)) % 360,
                                    60 + rng.bounded(120), 90 + rng.bounded(120));
        c2.setAlpha(150);
        tp.setPen(Qt::NoPen);
        tp.setBrush(c2);
        if (rng.bounded(2)) tp.drawEllipse(x, y, w, hh);
        else tp.drawRoundedRect(x, y, w, hh, 3, 3);
    }
    // silhouette horizon
    tp.setBrush(QColor(0, 0, 0, 90));
    QPolygonF hill;
    hill << QPointF(0, h);
    for (int x = 0; x <= tileW; x += 8)
        hill << QPointF(x, h - 6 - rng.bounded(h / 3));
    hill << QPointF(tileW, h);
    tp.drawPolygon(hill);

    // vignette
    QLinearGradient v(0, 0, 0, h);
    v.setColorAt(0, QColor(255, 255, 255, 26));
    v.setColorAt(0.5, QColor(0, 0, 0, 0));
    v.setColorAt(1, QColor(0, 0, 0, 70));
    tp.fillRect(pm.rect(), v);
    tp.end();

    m_thumbCache.insert(key, pm);
    return pm;
}

void EditorTimelineWidget::drawClip(QPainter &p, int index, bool ghost)
{
    Clip c = m_clips[index];
    if (ghost) { c.start = m_origStart; c.track = m_origTrack; c.length = m_origLength; }
    QRectF r = clipRect(c);
    if (r.right() < headerWidth() || r.left() > width()) return;

    const bool selected = m_selectedIds.contains(c.id) && !ghost;
    const bool hovered  = (index == m_hover) && !ghost;

    p.save();
    if (ghost) p.setOpacity(0.35);
    else if (c.track >= 0 && c.track < m_tracks.size() &&
             m_tracks[c.track].muted) p.setOpacity(0.45);
    // keep everything of the clip inside the content area
    p.setClipRect(QRectF(headerWidth(), 0, width() - headerWidth(), height()));

    QPainterPath path;
    path.addRoundedRect(r, 4, 4);

    const int nameBarH = 16;

    if (c.type == ClipType::Video) {
        // body
        p.fillPath(path, QColor(0x2a, 0x2a, 0x2c));
        // thumbnail filmstrip below the name bar
        QRectF body(r.left(), r.top() + nameBarH, r.width(), r.height() - nameBarH);
        if (body.height() > 4) {
            p.save();
            p.setClipRect(body, Qt::IntersectClip);
            // real filmstrip first: decoded frames pinned to their own
            // time position (frame -> x through the scene fps)
            const auto stripIt = m_film.constFind(c.id);
            const bool hasFilm = stripIt != m_film.constEnd() && !stripIt.value().isEmpty();
            if (hasFilm) {
                for (auto it = stripIt.value().constBegin();
                     it != stripIt.value().constEnd(); ++it) {
                    const double t = it.key() / m_fps;
                    const double x = timeToX(t);
                    const int w = qMax(2, int(it.value().width()
                                       * body.height() / it.value().height()));
                    if (x + w < body.left() || x > body.right()) { continue; }
                    p.drawImage(QRectF(x, body.top(), w, body.height()),
                                it.value());
                    p.setPen(QColor(0, 0, 0, 120));
                    p.drawLine(QPointF(x, body.top()), QPointF(x, body.bottom()));
                }
            } else {
                QPixmap tile = thumbnailTile(c, int(body.height()));
                for (double x = body.left(); x < body.right(); x += tile.width()) {
                    p.drawPixmap(QPointF(x, body.top()), tile);
                    p.setPen(QColor(0, 0, 0, 120));
                    p.drawLine(QPointF(x, body.top()), QPointF(x, body.bottom()));
                }
            }
            p.restore();
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
        if (body.width() > 4 && body.height() > 4) {
            p.setPen(QPen(cAudioWave, 1));
            const double mid = body.center().y();
            const auto waveIt = m_waves.constFind(c.id);
            const bool hasWave = waveIt != m_waves.constEnd() && !waveIt.value().isEmpty();
            const int n = int(body.width());
            for (int i = 0; i <= n; ++i) {
                double amp = 0.;
                if (hasWave) {
                    // real peaks: column time -> second + bucket lookup
                    const double t = xToTime(int(body.left()) + i);
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
                    quint32 hsh = quint32(c.id * 2654435761u) ^ quint32(i * 40503u);
                    hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                    amp = (hsh % 1000) / 1000.0;
                    amp = 0.15 + 0.85 * amp * (0.55 + 0.45 * qSin(i * 0.05));
                }
                const double x = body.left() + i;
                p.drawLine(QPointF(x, mid - amp * body.height() / 2),
                           QPointF(x, mid + amp * body.height() / 2));
            }
        }
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
    if (m_dropIllegal && index == m_dragClip && !ghost) {
        p.setPen(QPen(cPlayhead, 2));
        p.setBrush(QColor(cPlayhead.red(), cPlayhead.green(),
                          cPlayhead.blue(), 40));
        p.drawPath(path);
    }
    p.restore();
}

void EditorTimelineWidget::drawPlayhead(QPainter &p)
{
    int x = timeToX(m_playhead);
    if (x < headerWidth()) return;
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

// ---------------------------------------------------------------- picking

int EditorTimelineWidget::clipAt(const QPoint &pos, QRectF *rectOut) const
{
    // topmost track last drawn wins: iterate from end
    for (int i = m_clips.size() - 1; i >= 0; --i) {
        QRectF r = clipRect(m_clips[i]);
        if (r.contains(pos)) {
            if (rectOut) *rectOut = r;
            return i;
        }
    }
    return -1;
}

double EditorTimelineWidget::snapTime(double t, int ignoreClipIdx, bool *snappedOut) const
{
    double best = t;
    double bestDist = SNAP_PX / m_pxPerSec;
    bool snapped = false;

    auto consider = [&](double cand){
        double d = qAbs(cand - t);
        if (d < bestDist) { bestDist = d; best = cand; snapped = true; }
    };
    consider(m_playhead);
    for (int i = 0; i < m_clips.size(); ++i) {
        if (i == ignoreClipIdx) continue;
        consider(m_clips[i].start);
        consider(m_clips[i].start + m_clips[i].length);
    }
    // frame-quantized: the whole timeline lives on the scene frame grid
    best = qRound(best * m_fps) / m_fps;
    if (snappedOut) *snappedOut = snapped;
    return best;
}

bool EditorTimelineWidget::overlapsOnTrack(int track, double start, double len, int ignoreIdx) const
{
    for (int i = 0; i < m_clips.size(); ++i) {
        if (i == ignoreIdx || m_clips[i].track != track) continue;
        const Clip &o = m_clips[i];
        if (start < o.start + o.length - 1e-9 && o.start < start + len - 1e-9)
            return true;
    }
    return false;
}

// clips outside the moving set that the dragged clip + its group
// riders would collide with; the tangled exemption (press-time
// overlap with the moving set) lets legacy overlap layouts untangle
bool EditorTimelineWidget::overlapsOutsideMoving(const int dragIdx,
                                                 const int track,
                                                 const double start,
                                                 const double len) const
{
    for (int i = 0; i < m_clips.size(); ++i) {
        if (i == dragIdx) { continue; }
        if (m_drag == DragMode::MoveClip &&
                m_selectedIds.contains(m_clips[i].id)) { continue; } // rider
        if (m_tangled.contains(m_clips[i].id)) { continue; } // legacy knot
        const Clip &o = m_clips[i];
        if (o.track != track) { continue; }
        if (start < o.start + o.length - 1e-9 &&
                o.start < start + len - 1e-9) { return true; }
    }
    return false;
}

bool EditorTimelineWidget::dropLegal(const int dragIdx,
                                     const double newStart) const
{
    if (dragIdx < 0 || dragIdx >= m_clips.size()) { return false; }
    const Clip &c = m_clips[dragIdx];
    if (overlapsOutsideMoving(dragIdx, c.track, newStart, c.length)) { return false; }
    // group riders shift by the same delta on their own lanes
    const double delta = newStart - m_origStart;
    for (const auto &g : m_groupOrig) {
        if (g.idx < 0 || g.idx >= m_clips.size()) { continue; }
        const Clip &r = m_clips[g.idx];
        const double rs = qMax(0.0, g.origStart + delta);
        if (overlapsOutsideMoving(dragIdx, r.track, rs, r.length)) { return false; }
    }
    return true;
}

// corner buttons (+V / +A) in the ruler/header junction square
QRect EditorTimelineWidget::addTrackRect(const bool audio) const
{
    return QRect(audio ? 100 : 72, 6, 26, 18);
}

// nearest lane of the requested type for a y position (free drags
// clamp here instead of creating implicit lanes)
int EditorTimelineWidget::nearestLaneOfType(const int y,
                                            const ClipType type) const
{
    int best = -1;
    int bestDist = INT_MAX;
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].type != type) { continue; }
        const int mid = trackY(i) + m_tracks[i].height / 2;
        const int d = qAbs(y - mid);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------- events

// ---- editing tools ----

void EditorTimelineWidget::setTool(const EditTool tool)
{
    if (m_tool == tool) { return; }
    m_tool = tool;
    // a started gesture keeps running under the tool it began with
    // (only the hover cursor changes; the release finishes normally)
    m_rubber = false;
    applyToolCursor(mapFromGlobal(QCursor::pos()));
    update();
    emit toolChanged(static_cast<int>(tool));
}

bool EditorTimelineWidget::isTrackLocked(const int trackIdx) const
{
    return m_tracks.value(trackIdx).locked;
}

void EditorTimelineWidget::requestSplitAtPlayhead()
{
    emit splitAtPlayheadRequested();
}

void EditorTimelineWidget::applyToolCursor(const QPoint &pos)
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
        if (tr >= 0 && qAbs(pos.y() - (trackY(tr) + m_tracks[tr].height)) <= 4) {
            setCursor(Qt::SplitVCursor);
            return;
        }
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (m_tool == EditTool::Razor) {
        setCursor(m_razorCursor);
        return;
    }
    if (m_tool != EditTool::Select) {
        // track-select tools point at the clips they will collect
        QRectF r;
        setCursor(clipAt(pos, &r) >= 0 ? Qt::PointingHandCursor
                                       : Qt::ArrowCursor);
        return;
    }
    // select tool: trim edges / ruler hand / default
    QRectF r;
    const int idx = clipAt(pos, &r);
    if (idx >= 0 &&
        (qAbs(pos.x() - r.left()) <= TRIM_PX || qAbs(pos.x() - r.right()) <= TRIM_PX))
        setCursor(Qt::SizeHorCursor);
    else if (pos.y() <= rulerHeight())
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
}

// razor cut at the cursor/frame position: one clip for a plain click,
// every unlocked-lane clip under the time column for Shift (PR:
// shift-razor cuts all tracks). The cut is executed on the document
// side by EditorTimelineSync
void EditorTimelineWidget::razorCutAt(const QPoint &pos, const int idx,
                                      const bool allTracks)
{
    if (idx < 0 || idx >= m_clips.size()) { return; }
    if (isTrackLocked(m_clips[idx].track)) {
        emitLog(QStringLiteral("轨道已锁定，剪刀无效"));
        return;
    }
    // frame-quantized cut time (the timeline lives on the frame grid)
    const double sec = qRound(xToTime(pos.x()) * m_fps) / m_fps;
    QList<int> ids;
    for (int i = 0; i < m_clips.size(); ++i) {
        const Clip &c = m_clips[i];
        if (allTracks) {
            if (isTrackLocked(c.track)) { continue; }
            if (!(c.start < sec + 1e-9 && sec < c.start + c.length - 1e-9)) { continue; }
        } else if (i != idx) { continue; }
        ids << c.id;
    }
    if (ids.isEmpty()) { return; }
    // selection-neutral: drop the selection so the sync rebuild after
    // the cut does not resurrect both halves by name
    m_selectedIds.clear();
    emitSelectionSummary();
    emit razorCutRequested(ids, sec);
}

// PR track select: click collects the clicked clip plus every clip on
// the track in the tool direction from the click time; Shift widens to
// every track, Ctrl adds to the existing selection
void EditorTimelineWidget::trackSelectAt(const QPoint &pos, const int idx,
                                         const bool backward)
{
    if (idx < 0 || idx >= m_clips.size()) { return; }
    const double div = xToTime(pos.x());
    const int track = m_clips[idx].track;
    const bool allTracks = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    QSet<int> picked;
    for (const Clip &c : m_clips) {
        const bool inDir = backward ? (c.start <= div + 1e-9)
                                    : (c.start + c.length > div + 1e-9);
        if (!inDir) { continue; }
        if (!allTracks && c.track != track) { continue; }
        picked.insert(c.id);
    }
    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
        m_selectedIds.unite(picked);
    } else {
        m_selectedIds = picked;
    }
    emitSelectionSummary();
    emitLog(QStringLiteral("%1 %2 块（%3）")
            .arg(backward ? QStringLiteral("向左选择") : QStringLiteral("向右选择"))
            .arg(picked.size())
            .arg(allTracks ? QStringLiteral("全部轨道")
                           : m_tracks.value(track).name));
}

void EditorTimelineWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    m_pressPos = e->pos();

    // heal a drag whose release was lost (dock hidden mid-drag, alt-tab
    // with button held): a fresh press always starts from a clean state
    if (m_drag != DragMode::None) {
        qDebug("[ETL] press heals stale drag=%d clip=%d",
               static_cast<int>(m_drag), m_dragClip);
        m_drag = DragMode::None;
        m_dragClip = -1;
        m_dragTrack = -1;
        m_snapTarget = -1.0;
    }

    if (e->button() == Qt::MiddleButton) {
        // reserved: pan view
        return;
    }
    if (e->button() != Qt::LeftButton) return;

    // clips win over the playhead: a press on a block must never start a
    // playhead drag, even when the block sits under the playhead line
    QRectF r;
    int idx = clipAt(e->pos(), &r);
    qDebug("[ETL] press %d,%d hit=%d clips=%d tracks=%d scroll=%.1fs px/s=%.1f",
           e->pos().x(), e->pos().y(), idx, m_clips.size(), m_tracks.size(),
           m_scrollSec, m_pxPerSec);

    // corner square (+V / +A track buttons)
    if (e->pos().x() < headerWidth() && e->pos().y() <= rulerHeight()) {
        if (addTrackRect(false).contains(e->pos())) {
            emit trackAddRequested(false);
        } else if (addTrackRect(true).contains(e->pos())) {
            emit trackAddRequested(true);
        }
        return;
    }

    // ---- track header area: height resize / mute / lock ----
    if (e->pos().x() < headerWidth() && e->pos().y() > rulerHeight()) {
        const int tr = trackAtY(e->pos().y());
        if (tr >= 0) {
            // bottom edge of the header row = lane height resize
            if (qAbs(e->pos().y() - (trackY(tr) + m_tracks[tr].height)) <= 4) {
                m_drag = DragMode::TrackHeight;
                m_dragTrack = tr;
                m_pressTrackHeight = m_tracks[tr].height;
                return;
            }
            if (muteBadgeRect(tr).contains(e->pos())) {
                // Shift = every track of the same type (kdenlive)
                emit trackMuteToggleRequested(
                            tr, e->modifiers() & Qt::ShiftModifier);
                return;
            }
            if (lockBadgeRect(tr).contains(e->pos())) {
                m_tracks[tr].locked = !m_tracks[tr].locked;
                emit trackLockChanged(tr, m_tracks[tr].locked);
                update();
                return;
            }
        }
        return; // plain header click: nothing (double-click renames)
    }

    // playhead: ruler area or near playhead line (when no clip is there)
    int phx = timeToX(m_playhead);
    if (idx < 0 && e->pos().x() >= headerWidth() &&
            (e->pos().y() <= rulerHeight() || qAbs(e->pos().x() - phx) <= 4)) {
        m_drag = DragMode::Playhead;
        m_playhead = qMax(0.0, xToTime(e->pos().x()));
        emitLog(QStringLiteral("playhead -> %1").arg(timecode(m_playhead)));
        update();
        return;
    }

    if (idx >= 0 && m_tool != EditTool::Select) {
        // editing tools replace the plain-click clip interaction
        if (m_tool == EditTool::Razor) {
            razorCutAt(e->pos(), idx, e->modifiers() & Qt::ShiftModifier);
        } else {
            trackSelectAt(e->pos(), idx, m_tool == EditTool::TrackBackward);
        }
        update();
        return;
    }

    if (idx >= 0) {
        const Clip &c = m_clips[idx];
        // multi-select: ctrl toggles membership; a plain press on an
        // already-selected clip keeps the whole selection (group move)
        if (e->modifiers() & Qt::ControlModifier) {
            if (m_selectedIds.contains(c.id)) { m_selectedIds.remove(c.id); }
            else { m_selectedIds.insert(c.id); }
        } else if (!m_selectedIds.contains(c.id)) {
            m_selectedIds.clear();
            m_selectedIds.insert(c.id);
        }
        emitSelectionSummary();

        const bool laneLocked = m_tracks.value(c.track).locked;
        m_dragClip = idx;
        m_origStart = c.start;
        m_origLength = c.length;
        m_origTrack = c.track;
        m_dropIllegal = false;
        // group move: snapshot every other selected clip's start so the
        // whole selection rides the same time delta
        m_groupOrig.clear();
        if (m_selectedIds.count() > 1) {
            for (int i = 0; i < m_clips.size(); ++i) {
                if (i != idx && m_selectedIds.contains(m_clips[i].id)) {
                    m_groupOrig.append({i, m_clips[i].start});
                }
            }
        }
        // press-time tangle snapshot: clips outside the moving set that
        // already overlap it (legacy overlap layouts) are exempt from
        // the drop legality check, so knots can still be dragged apart
        m_tangled.clear();
        {
            const auto tangleOn = [this](const int lane, const double s,
                                         const double l) {
                for (const Clip &o : m_clips) {
                    if (m_selectedIds.contains(o.id)) { continue; }
                    if (o.track != lane) { continue; }
                    if (s < o.start + o.length - 1e-9 &&
                            o.start < s + l - 1e-9) {
                        m_tangled.insert(o.id);
                    }
                }
            };
            tangleOn(c.track, c.start, c.length);
            for (const auto &g : m_groupOrig) {
                if (g.idx >= 0 && g.idx < m_clips.size()) {
                    tangleOn(m_clips[g.idx].track,
                             m_clips[g.idx].start,
                             m_clips[g.idx].length);
                }
            }
        }

        if (laneLocked) {
            m_drag = DragMode::None; // selectable but not editable
        } else {
            bool nearL = qAbs(e->pos().x() - r.left()) <= TRIM_PX;
            bool nearR = qAbs(e->pos().x() - r.right()) <= TRIM_PX;
            if (nearL && !nearR) {
                m_drag = DragMode::TrimLeft;
            } else if (nearR) {
                m_drag = DragMode::TrimRight;
                // snapshot same-track starts for the magnetic follow; the
                // follow is recomputed from this every move so it is fully
                // reversible while dragging
                m_trimSnap.clear();
                for (int i = 0; i < m_clips.size(); ++i) {
                    if (i != idx && m_clips[i].track == c.track) {
                        m_trimSnap.append({i, m_clips[i].start});
                    }
                }
            } else {
                m_drag = DragMode::MoveClip;
                m_grabOffsetSec = xToTime(e->pos().x()) - c.start;
            }
        }
    } else if (e->pos().x() >= headerWidth()) {
        // empty content area
        if (!(e->modifiers() & Qt::ControlModifier)) {
            m_selectedIds.clear();
            emitSelectionSummary();
        }
        // rubber band is a selection-tool gesture; with razor or track
        // tools a plain empty click just clears the selection
        if (m_tool == EditTool::Select) {
            m_rubber = true;
            m_rubberStart = e->pos();
            m_drag = DragMode::None;
        }
    }
    update();
}

void EditorTimelineWidget::emitSelectionSummary()
{
    if (m_selectedIds.isEmpty()) { emit selectionChanged(QString()); return; }
    QStringList names;
    for (const Clip &c : m_clips) {
        if (m_selectedIds.contains(c.id)) {
            names << c.name;
            if (names.count() >= 3) { names << QStringLiteral("…"); break; }
        }
    }
    emit selectionChanged(names.join(QStringLiteral(", ")));
}

void EditorTimelineWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_drag == DragMode::None) {
        // hover + cursor feedback
        QRectF r;
        int idx = clipAt(e->pos(), &r);
        const bool hoverChanged = idx != m_hover;
        m_hover = idx;
        m_hoverPos = e->pos();
        applyToolCursor(e->pos());
        if (hoverChanged || m_tool == EditTool::Razor) { update(); }
        return;
    }

    // rubber band selection: live update, finalize on release
    if (m_rubber) {
        update();
        return;
    }

    m_snapTarget = -1.0;
    double t = xToTime(e->pos().x());

    switch (m_drag) {
    case DragMode::Playhead:
        m_playhead = qMax(0.0, t);
        break;
    case DragMode::TrackHeight: {
        if (m_dragTrack >= 0 && m_dragTrack < m_tracks.size()) {
            const int dy = e->pos().y() - m_pressPos.y();
            m_tracks[m_dragTrack].height =
                    qBound(36, m_pressTrackHeight + dy, 220);
        }
        update();
        return; // header gestures never auto-scroll the content
    }
    case DragMode::MoveClip: {
        Clip &c = m_clips[m_dragClip];
        double newStart = qMax(0.0, t - m_grabOffsetSec);
        bool snapped = false;
        double snappedT = snapTime(newStart, m_dragClip, &snapped);
        if (snapped) { newStart = snappedT; m_snapTarget = snappedT; }

        // lane under the cursor, clamped to the nearest same-type lane:
        // tracks are explicit entities now, hovering outside never
        // spawns a throwaway lane
        int tr = trackAtY(e->pos().y());
        if (tr < 0 || m_tracks[tr].type != c.type) {
            tr = nearestLaneOfType(e->pos().y(), c.type);
        }
        const bool laneOk = tr >= 0 && !m_tracks[tr].locked;

        // overlap hard constraint: an illegal candidate keeps the last
        // legal state and flags the dragged clip red
        const int prevTrack = c.track;
        const double prevStart = c.start;
        if (laneOk) { c.track = tr; }
        c.start = newStart;
        if (!(laneOk && dropLegal(m_dragClip, newStart))) {
            c.track = prevTrack;
            c.start = prevStart;
            m_dropIllegal = true;
            m_snapTarget = -1.0;
        } else {
            m_dropIllegal = false;
            // group move: every other selected clip rides the same time
            // delta (vertical lane changes stay single-clip on purpose)
            if (!m_groupOrig.isEmpty()) {
                const double delta = newStart - m_origStart;
                for (const auto &g : m_groupOrig) {
                    if (g.idx >= 0 && g.idx < m_clips.size()) {
                        m_clips[g.idx].start = qMax(0.0, g.origStart + delta);
                    }
                }
            }
        }
        break;
    }
    case DragMode::TrimLeft: {
        Clip &c = m_clips[m_dragClip];
        double end = m_origStart + m_origLength;
        double ns = qBound(0.0, t, end - MIN_CLIP_LEN);
        bool snapped = false;
        double sT = snapTime(ns, m_dragClip, &snapped);
        if (snapped) { ns = sT; m_snapTarget = sT; }
        // neighbor edge: the left handle cannot cross the previous clip
        // on the lane (clips tangled with this one stay trimmable)
        double lo = 0.0;
        for (int i = 0; i < m_clips.size(); ++i) {
            if (i == m_dragClip) { continue; }
            const Clip &o = m_clips[i];
            if (o.track != c.track || m_selectedIds.contains(o.id)) { continue; }
            if (o.start + o.length <= m_origStart + 1e-9) {
                lo = qMax(lo, o.start + o.length);
            }
        }
        lo = qMin(lo, end - MIN_CLIP_LEN);
        ns = qMax(ns, lo);
        c.start = ns;
        c.length = end - ns;
        break;
    }
    case DragMode::TrimRight: {
        Clip &c = m_clips[m_dragClip];
        double ne = qMax(t, m_origStart + MIN_CLIP_LEN);
        bool snapped = false;
        double sT = snapTime(ne, m_dragClip, &snapped);
        if (snapped) { ne = sT; m_snapTarget = sT; }
        // neighbor edge: the right handle cannot cross the next clip's
        // start on the lane
        double hi = double(INT_MAX);
        for (int i = 0; i < m_clips.size(); ++i) {
            if (i == m_dragClip) { continue; }
            const Clip &o = m_clips[i];
            if (o.track != c.track || m_selectedIds.contains(o.id)) { continue; }
            if (o.start >= m_origStart - 1e-9) { hi = qMin(hi, o.start); }
        }
        hi = qMax(hi, m_origStart + MIN_CLIP_LEN);
        ne = qMin(ne, hi);
        c.length = ne - c.start;
        // magnetic follow: shortening the out point slides attached
        // neighbours left; Alt keeps them in place for a plain trim
        applyMagneticFollow(!(e->modifiers() & Qt::AltModifier));
        break;
    }
    default: break;
    }

    // edge auto-scroll while dragging clips (never for the playhead or
    // header gestures)
    if (m_drag == DragMode::MoveClip || m_drag == DragMode::TrimLeft ||
            m_drag == DragMode::TrimRight) {
        if (e->pos().x() > width() - 24) m_scrollSec += 20 / m_pxPerSec;
        else if (e->pos().x() < headerWidth() + 24) m_scrollSec = qMax(0.0, m_scrollSec - 20 / m_pxPerSec);
        updateScrollBar();
    }
    update();
}

void EditorTimelineWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;

    if (m_rubber) {
        m_rubber = false;
        const QRect band = QRect(m_rubberStart, e->pos()).normalized();
        bool changed = false;
        for (const Clip &c : m_clips) {
            if (band.intersects(clipRect(c).toRect())) {
                if (!m_selectedIds.contains(c.id)) {
                    m_selectedIds.insert(c.id);
                    changed = true;
                }
            }
        }
        if (changed) { emitSelectionSummary(); }
        update();
        return;
    }

    if (m_drag == DragMode::TrackHeight) {
        if (m_dragTrack >= 0 && m_dragTrack < m_tracks.size()) {
            emit trackHeightChanged(m_dragTrack,
                                    m_tracks[m_dragTrack].height);
        }
        m_drag = DragMode::None;
        m_dragTrack = -1;
        update();
        return;
    }

    if (m_drag != DragMode::None && m_dragClip >= 0 && m_dragClip < m_clips.size()) {
        const Clip &c = m_clips[m_dragClip];
        if (qAbs(c.start - m_origStart) > 1e-6 || qAbs(c.length - m_origLength) > 1e-6 ||
            c.track != m_origTrack) {
            QString what = m_drag == DragMode::MoveClip ? QStringLiteral("move")
                         : m_drag == DragMode::TrimLeft ? QStringLiteral("trim-left")
                                                        : QStringLiteral("trim-right");
            emitLog(QStringLiteral("%1 clip \"%2\": [%3 → %4] track %5")
                    .arg(what, c.name, timecode(c.start), timecode(c.start + c.length))
                    .arg(m_tracks[c.track].name));
            emitSelectionSummary();
        }
    }
    m_drag = DragMode::None;
    m_dragClip = -1;
    m_snapTarget = -1.0;
    m_dropIllegal = false;
    m_tangled.clear();
    m_groupOrig.clear();
    update();
}

void EditorTimelineWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    // double click track header name: rename the lane
    if (e->pos().x() < headerWidth() && e->pos().y() > rulerHeight()) {
        const int lane = trackAtY(e->pos().y());
        if (lane >= 0 && !muteBadgeRect(lane).contains(e->pos())
                && !lockBadgeRect(lane).contains(e->pos())) {
            renameLaneDialog(lane);
        }
        return;
    }
    // double click ruler: move playhead without drag
    if (e->pos().y() <= rulerHeight()) {
        m_playhead = qMax(0.0, xToTime(e->pos().x()));
        update();
    }
}

void EditorTimelineWidget::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        double factor = e->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
        applyZoom(factor, e->position().x());
    } else {
        double delta = -e->angleDelta().y() / 120.0; // steps
        m_scrollSec = qMax(0.0, m_scrollSec + delta * 40.0 / m_pxPerSec);
        clampView();
        updateScrollBar();
    }
    update();
    e->accept();
}

void EditorTimelineWidget::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
            && !m_selectedIds.isEmpty()) {
        requestDelete(e->modifiers() & Qt::ShiftModifier);
        return;
    }
    if (e->key() == Qt::Key_S || e->key() == Qt::Key_Slash) {
        // no-selection press still splits: the sync falls back to every
        // unlocked-lane clip under the playhead (CapCut 分割 semantics)
        emit splitAtPlayheadRequested();
        return;
    }
    if (e->key() == Qt::Key_A && e->modifiers() & Qt::ControlModifier) {
        m_selectedIds.clear();
        for (const Clip &c : m_clips) { m_selectedIds.insert(c.id); }
        emitSelectionSummary();
        update();
        return;
    }
    // PR-style tool keys: V select, B razor, A track-select forward,
    // Shift+A track-select backward
    if (e->key() == Qt::Key_V && e->modifiers() == Qt::NoModifier) {
        setTool(EditTool::Select);
        return;
    }
    if (e->key() == Qt::Key_B && e->modifiers() == Qt::NoModifier) {
        setTool(EditTool::Razor);
        return;
    }
    if (e->key() == Qt::Key_A && e->modifiers() == Qt::NoModifier) {
        setTool(EditTool::TrackForward);
        return;
    }
    if (e->key() == Qt::Key_A && e->modifiers() == Qt::ShiftModifier) {
        setTool(EditTool::TrackBackward);
        return;
    }
    QWidget::keyPressEvent(e);
}

void EditorTimelineWidget::leaveEvent(QEvent *)
{
    if (m_hover != -1) { m_hover = -1; }
    m_hoverPos = QPoint(-1, -1);
    update();
}

void EditorTimelineWidget::hideEvent(QHideEvent *)
{
    // dock toggled off: drop any drag state so reopening starts clean
    if (m_drag != DragMode::None) {
        qDebug("[ETL] hidden with active drag=%d, resetting",
               static_cast<int>(m_drag));
    }
    m_drag = DragMode::None;
    m_dragClip = -1;
    m_dragTrack = -1;
    m_snapTarget = -1.0;
}

void EditorTimelineWidget::resizeEvent(QResizeEvent *)
{
    updateScrollBar();
    emit viewChanged();
}

// ---------------------------------------------------------------- public ops

void EditorTimelineWidget::removeSelectedClip()
{
    // bridge era: deletion is document-side (undoable); the widget only
    // mirrors the document through the sync rebuild
    requestDelete(false);
}

void EditorTimelineWidget::applyZoom(double factor, int anchorX)
{
    double anchorTime = xToTime(anchorX);
    double old = m_pxPerSec;
    m_pxPerSec = qBound(4.0, m_pxPerSec * factor, 1200.0);
    // keep anchorTime under the cursor
    m_scrollSec = anchorTime - (anchorX - headerWidth()) / m_pxPerSec;
    if (m_pxPerSec != old) {
        emitLog(QStringLiteral("zoom %1 px/s").arg(m_pxPerSec, 0, 'f', 1));
    }
    clampView();
    updateScrollBar();
    emit viewChanged();
    update();
}

void EditorTimelineWidget::zoomIn()  { applyZoom(1.3, headerWidth() + (width() - headerWidth()) / 2); }
void EditorTimelineWidget::zoomOut() { applyZoom(1.0 / 1.3, headerWidth() + (width() - headerWidth()) / 2); }

void EditorTimelineWidget::zoomFit()
{
    double dur = contentDuration();
    int avail = width() - headerWidth();
    if (dur > 0 && avail > 0)
        m_pxPerSec = qBound(4.0, avail / dur, 1200.0);
    m_scrollSec = 0;
    updateScrollBar();
    update();
}

void EditorTimelineWidget::clampView()
{
    double maxScroll = qMax(0.0, contentDuration() - (width() - headerWidth()) / m_pxPerSec);
    m_scrollSec = qBound(0.0, m_scrollSec, maxScroll + 2.0);
}

void EditorTimelineWidget::updateScrollBar()
{
    if (!m_scrollBar) return;
    clampView();
    emit viewChanged();
    double content = contentDuration();
    double page = (width() - headerWidth()) / m_pxPerSec;
    m_scrollBar->setRange(0, int(qMax(0.0, content - page) * 100));
    m_scrollBar->setPageStep(int(page * 100));
    m_scrollBar->setValue(int(m_scrollSec * 100));
    m_scrollBar->setSingleStep(int(0.5 * 100));
}

void EditorTimelineWidget::emitLog(const QString &msg)
{
    emit logMessage(QStringLiteral("[%1] %2")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), msg));
}

// ================= friction semantic bridge =================
// Appended section; every line above is the byte-level TimelineDemo
// port and stays untouched. Driven by EditorTimelineSync.

void EditorTimelineWidget::clearAllClips()
{
    // remember the selection so the sync-driven rebuild can restore it
    m_keepSelNames.clear();
    for (const Clip &c : m_clips) {
        if (m_selectedIds.contains(c.id)) { m_keepSelNames.insert(c.name); }
    }
    m_selectedIds.clear();
    m_hover = -1;
    m_drag = DragMode::None;
    m_dragClip = -1;
    m_dragTrack = -1;
    m_snapTarget = -1.0;
    m_clips.clear();
    m_thumbCache.clear();
    m_realThumbs.clear();
    m_realScaled.clear();
    m_film.clear();
    m_waves.clear();
    emit selectionChanged(QString());
    updateScrollBar();
    update();
}

int EditorTimelineWidget::appendClip(const QString &name,
                                     const double startSec,
                                     const double lengthSec,
                                     const bool audio,
                                     const int track)
{
    Clip c;
    c.id = m_nextId++;
    c.name = name;
    c.type = audio ? ClipType::Audio : ClipType::Video;
    c.track = m_tracks.isEmpty() ? 0 : qBound(0, track, m_tracks.size() - 1);
    c.start = qMax(0.0, startSec);
    c.length = qMax(MIN_CLIP_LEN, lengthSec);
    c.hueSeed = c.id * 37;
    m_clips.push_back(c);
    if (m_keepSelNames.contains(c.name)) {
        m_selectedIds.insert(c.id);
    }
    updateScrollBar();
    update();
    return c.id;
}

void EditorTimelineWidget::setTracks(const QVector<TrackInfo> &tracks)
{
    m_tracks.clear();
    for (const auto &t : tracks) {
        Track lane;
        lane.id = t.id;
        lane.name = t.name;
        lane.type = t.audio ? ClipType::Audio : ClipType::Video;
        lane.locked = t.locked;
        lane.muted = t.muted;
        lane.height = t.height > 0 ? t.height : (t.audio ? 52 : 60);
        m_tracks.append(lane);
    }
    updateScrollBar();
    update();
}

void EditorTimelineWidget::setPlayheadSec(const double t)
{
    m_playhead = qMax(0.0, t);
    update();
}

QVector<EditorTimelineWidget::ClipInfo> EditorTimelineWidget::allClips() const
{
    QVector<ClipInfo> out;
    out.reserve(m_clips.size());
    for (const Clip &c : m_clips) {
        ClipInfo info;
        info.id = c.id;
        info.name = c.name;
        info.start = c.start;
        info.length = c.length;
        info.track = c.track;
        info.audio = c.type == ClipType::Audio;
        out.append(info);
    }
    return out;
}

int EditorTimelineWidget::videoTrackCount() const
{
    int n = 0;
    while (n < m_tracks.size() && m_tracks[n].type == ClipType::Video) ++n;
    return n;
}

void EditorTimelineWidget::contextMenuEvent(QContextMenuEvent *e)
{
    // ruler: marker management at the clicked frame
    if (e->pos().y() <= rulerHeight() && e->pos().x() >= headerWidth()) {
        const int frame = qRound(xToTime(e->pos().x()) * m_fps);
        QMenu menu(this);
        QAction *add = menu.addAction(tr("在此处添加标记"));
        int nearest = -1;
        int bestDist = 12 * m_fps / m_pxPerSec + 8;
        for (const auto &mark : m_markers) {
            const int d = qAbs(mark.first - frame);
            if (d < bestDist) { bestDist = d; nearest = mark.first; }
        }
        QAction *remove = menu.addAction(tr("移除最近的标记"));
        remove->setEnabled(nearest >= 0);
        QAction *act = menu.exec(e->globalPos());
        if (act == add) { emit markerAddRequested(frame); }
        else if (act == remove && nearest >= 0) {
            emit markerRemoveRequested(nearest);
        }
        return;
    }

    // track header: lifecycle menu (explicit tracks)
    if (e->pos().x() < headerWidth() && e->pos().y() > rulerHeight()) {
        const int lane = trackAtY(e->pos().y());
        if (lane < 0) { QWidget::contextMenuEvent(e); return; }
        const bool audio = m_tracks[lane].type == ClipType::Audio;
        QMenu menu(this);
        QAction *add = menu.addAction(audio ? tr("添加音频轨")
                                            : tr("添加视频轨"));
        QAction *del = menu.addAction(tr("删除轨道（仅空轨可删）"));
        QAction *rename = menu.addAction(tr("重命名轨道"));
        QAction *act = menu.exec(e->globalPos());
        if (act == add) { emit trackAddRequested(audio); }
        else if (act == del) { emit trackRemoveRequested(lane); }
        else if (act == rename) { renameLaneDialog(lane); }
        return;
    }

    const int idx = clipAt(e->pos());
    if (idx < 0) { QWidget::contextMenuEvent(e); return; }
    if (!m_selectedIds.contains(m_clips[idx].id)) {
        m_selectedIds.clear();
        m_selectedIds.insert(m_clips[idx].id);
        emitSelectionSummary();
        update();
    }
    const Clip c = m_clips.value(idx);
    const int src = c.track;
    const bool upOk = src > 0 && m_tracks[src - 1].type == c.type;
    const bool downOk = src + 1 < m_tracks.size()
            && m_tracks[src + 1].type == c.type;

    QMenu menu(this);
    // NLE editing section (always available)
    QAction *splitHere = menu.addAction(tr("在此处分割"));
    QAction *del = menu.addAction(tr("删除"));
    QAction *rippleDel = menu.addAction(tr("波纹删除"));
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
    if (act == splitHere) { razorCutAt(e->pos(), idx, false); }
    else if (act == del) { requestDelete(false); }
    else if (act == rippleDel) { requestDelete(true); }
    else if (act == back) { trackSelectAt(e->pos(), idx, true); }
    else if (act == backAll) { trackSelectAll(e->pos(), true); }
    else if (act == fwd) { trackSelectAt(e->pos(), idx, false); }
    else if (act == fwdAll) { trackSelectAll(e->pos(), false); }
    else if (act == up && upOk) moveClipToLane(idx, -1);
    else if (act == down && downOk) moveClipToLane(idx, 1);
    update();
}

// shared by the header double-click and the context menu
void EditorTimelineWidget::renameLaneDialog(const int lane)
{
    if (lane < 0 || lane >= m_tracks.size()) { return; }
    bool ok = false;
    const QString name = QInputDialog::getText(
                this, tr("重命名轨道"), tr("轨道名称:"),
                QLineEdit::Normal, m_tracks[lane].name, &ok);
    if (ok && !name.trimmed().isEmpty()) {
        m_tracks[lane].name = name.trimmed();
        emit trackRenameRequested(lane, m_tracks[lane].name);
        update();
    }
}

// direction-only variant for the context menu (no clip hit needed):
// every clip on every track before/after the click time
void EditorTimelineWidget::trackSelectAll(const QPoint &pos,
                                          const bool backward)
{
    const double div = xToTime(pos.x());
    QSet<int> picked;
    for (const Clip &c : m_clips) {
        const bool inDir = backward ? (c.start <= div + 1e-9)
                                    : (c.start + c.length > div + 1e-9);
        if (inDir) { picked.insert(c.id); }
    }
    m_selectedIds = picked;
    emitSelectionSummary();
    emitLog(QStringLiteral("%1 %2 块（全部轨道）")
            .arg(backward ? QStringLiteral("向左选择") : QStringLiteral("向右选择"))
            .arg(picked.size()));
}

// context-menu lane hop: same-type neighbour lane only, refused on
// collision or a locked target (the writeback turns it into a plain
// trackId change, no row surgery)
void EditorTimelineWidget::moveClipToLane(const int clipIdx, const int dir)
{
    if (clipIdx < 0 || clipIdx >= m_clips.size()) { return; }
    const Clip c = m_clips.value(clipIdx);
    int dst = c.track + dir;
    if (dst < 0 || dst >= m_tracks.size()) { return; }
    if (m_tracks[dst].type != c.type) { return; }
    if (m_tracks[dst].locked) {
        emitLog(QStringLiteral("目标轨道已锁定"));
        return;
    }
    if (overlapsOutsideMoving(clipIdx, dst, c.start, c.length)) {
        emitLog(QStringLiteral("目标位置与现有块重叠"));
        return;
    }
    m_clips[clipIdx].track = dst;
    emitLog(QStringLiteral("移动块 \"%1\" 到 %2")
            .arg(c.name, m_tracks[dst].name));
    emit trackLayoutChanged();
}

void EditorTimelineWidget::setClipThumbnail(const int clipId, const QImage &image)
{
    if (image.isNull()) return;
    m_realThumbs.insert(clipId, image);
    m_realScaled.remove(clipId);
    update();
}

void EditorTimelineWidget::applyMagneticFollow(const bool active)
{
    if (m_drag != DragMode::TrimRight || m_dragClip < 0 ||
            m_dragClip >= m_clips.size()) { return; }
    // live follow only exists in magnetic mode (Alt temporarily suspends);
    // suspended or off -> plain trim, all snapshots restore
    if (!mMagnetic || !active) {
        for (const auto &snap : m_trimSnap) {
            if (snap.idx >= 0 && snap.idx < m_clips.size()) {
                m_clips[snap.idx].start = snap.start;
            }
        }
        return;
    }
    const Clip &c = m_clips[m_dragClip];
    const double oldEnd = m_origStart + m_origLength;
    const double newEnd = c.start + c.length;
    // tape model: every same-track clip behind the trimmed one shifts by
    // the same amount the out point moved - shortening slides the chain
    // left onto the new out point (no gap), lengthening pushes it right
    // (no overlap). Their mutual spacing and overlaps survive the shift,
    // and the snapshot base keeps every move idempotent/reversible.
    const double shift = newEnd - oldEnd;
    for (const auto &snap : m_trimSnap) {
        if (snap.idx < 0 || snap.idx >= m_clips.size()) { continue; }
        Clip &f = m_clips[snap.idx];
        if (qAbs(shift) > 1e-9 && snap.start >= oldEnd - 1e-9) {
            f.start = qMax(0.0, snap.start + shift);
        } else {
            f.start = snap.start;
        }
    }
}

void EditorTimelineWidget::setMagnetic(const bool on)
{
    if (mMagnetic == on) { return; }
    mMagnetic = on;
    if (!on) { return; }
    // turning it on enforces the no-gap invariant right away; the layout
    // signal lets the bridge persist the moved starts (undoable)
    const int moved = compactTrackGaps();
    if (moved > 0) {
        emitLog(QStringLiteral("magnetic on: %1 clip(s) slid closed").arg(moved));
        emit trackLayoutChanged();
    } else {
        emitLog(QStringLiteral("magnetic on: tracks already gap-free"));
    }
}

int EditorTimelineWidget::compactTrackGaps()
{
    // per track, in start order: a clip that begins after the previous
    // one ends slides left onto that out point; overlaps (merged tracks)
    // are kept as-is, so only genuine gaps close
    int moved = 0;
    for (int t = 0; t < m_tracks.size(); ++t) {
        QVector<int> order;
        for (int i = 0; i < m_clips.size(); ++i) {
            if (m_clips[i].track == t) { order.append(i); }
        }
        if (order.size() < 2) { continue; }
        std::sort(order.begin(), order.end(), [this](const int a, const int b) {
            return m_clips[a].start < m_clips[b].start;
        });
        double cursor = -1.0;
        for (const int i : order) {
            Clip &c = m_clips[i];
            if (cursor >= 0.0 && c.start > cursor + 1e-9) {
                c.start = cursor;
                ++moved;
            }
            cursor = qMax(cursor, c.start + c.length);
        }
    }
    if (moved > 0) { update(); }
    return moved;
}

void EditorTimelineWidget::refreshThemeColors()
{
    // follow the app theme: accent drives selection/highlight, its
    // darker shade the video clip name bars (was a hard-coded green)
    const QColor accent = ThemeSupport::getThemeHighlightColor();
    const QColor accentDark = ThemeSupport::getThemeHighlightDarkerColor();
    if (cAccent != accent) { cAccent = accent; }
    if (cVideoBar != accentDark) { cVideoBar = accentDark; }
}

QList<int> EditorTimelineWidget::selectedClipIds() const
{
    QList<int> ids;
    for (const Clip &c : m_clips) {
        if (m_selectedIds.contains(c.id)) { ids << c.id; }
    }
    return ids;
}

void EditorTimelineWidget::setMarkers(const QVector<QPair<int, QString>> &markers)
{
    m_markers = markers;
    update();
}

void EditorTimelineWidget::setClipThumbFrame(const int clipId,
                                             const int absFrame,
                                             const QImage &image)
{
    if (image.isNull()) { return; }
    auto &strip = m_film[clipId];
    strip.insert(absFrame, image);
    update();
}

void EditorTimelineWidget::setClipWave(const int clipId, const int absSecond,
                                       const QVector<qreal> &peaks)
{
    if (peaks.isEmpty()) { return; }
    auto &wave = m_waves[clipId];
    if (wave.contains(absSecond) && wave.value(absSecond).size() == peaks.size()) {
        return; // unchanged: no repaint storm on every rebuild
    }
    wave.insert(absSecond, peaks);
    update();
}

double EditorTimelineWidget::viewStartSec() const
{
    return xToTime(headerWidth() + 1);
}

double EditorTimelineWidget::viewEndSec() const
{
    return xToTime(width() - 1);
}

QRect EditorTimelineWidget::muteBadgeRect(const int trackIdx) const
{
    const int top = trackY(trackIdx);
    const int h = m_tracks.value(trackIdx).height;
    return QRect(headerWidth() - 58, top + (h - 20) / 2, 20, 20);
}

QRect EditorTimelineWidget::lockBadgeRect(const int trackIdx) const
{
    const int top = trackY(trackIdx);
    const int h = m_tracks.value(trackIdx).height;
    return QRect(headerWidth() - 32, top + (h - 20) / 2, 20, 20);
}

void EditorTimelineWidget::requestDelete(const bool ripple)
{
    if (m_selectedIds.isEmpty()) { return; }
    emit deleteRequested(ripple);
}
