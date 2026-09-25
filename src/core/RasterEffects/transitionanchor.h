/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software: you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
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

#ifndef TRANSITIONANCHOR_H
#define TRANSITIONANCHOR_H

#include <QtMath>

struct BoxRenderData;

// CapCut-style clip transition anchoring: relFrame is the clip-local
// frame (0 = first visible frame of the clip), total is the clip length
// in frames (nleTransitionTotalFrames). Returns the transition
// openness 0..1 at this frame: the fade-in window ramps
// 0 -> 1, the fade-out window ramps 1 -> 0, the middle holds 1.
// fadeIn/fadeOut <= 0.5 frames disable that end.
inline qreal nleTransitionOpenness(const qreal relFrame, const qreal total,
                                   const qreal fadeIn, const qreal fadeOut)
{
    if (total < 1.) { return 1.; }
    const qreal a = fadeIn > 0.5 ? relFrame / fadeIn : 1.;
    const qreal b = fadeOut > 0.5 ? (total - relFrame) / fadeOut : 1.;
    return qBound(0., qMin(a, b), 1.);
}

// The render pipeline hands effects the RAW animator relFrame, where
// the layer's duration rect sits at [minRel..maxRel] and minRel equals
// the layer's placement frame at creation time (e.g. 300 for a clip
// imported mid-timeline; later whole-clip moves change relShift, never
// minRel). Transition progress must instead count from the clip's
// first visible frame: convert to clip-local coordinates. Without
// this, headOpen = 300/fadeIn clamps to 1 on every frame and the
// transition renders as identity (the applied-but-invisible bug).
// The effect-preview path passes no BoxRenderData and already sweeps
// local frames 0..N, so fall back to the raw frame there.
qreal nleTransitionClipRelFrame(const qreal relFrame,
                                const BoxRenderData * const data);

// clip length for the anchor: from the layer's duration rect; the
// effect-preview path passes no BoxRenderData, so fall back to a 2s
// loop whose relFrame sweep produces a live in -> hold -> out cycle
qreal nleTransitionTotalFrames(const BoxRenderData * const data);

#endif // TRANSITIONANCHOR_H
