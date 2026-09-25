/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
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

#ifndef BLURDISSOLVEEFFECT_H
#define BLURDISSOLVEEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style blur dissolve transition: over the head window the
// clip fades in from heavy blur + full transparency to sharp + fully
// opaque; the tail window blurs back out on remove. A two-pass
// separable box blur on the premultiplied pixels approximates the
// gaussian well enough at transition speeds.
class BlurDissolveEffect : public RasterEffect {
public:
    BlurDissolveEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mMaxRadius;
};

#endif // BLURDISSOLVEEFFECT_H
