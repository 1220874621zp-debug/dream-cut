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
// but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#ifndef BOOKFLIPEFFECT_H
#define BOOKFLIPEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style book flip (gl-transitions "BookFlip" port, incoming
// side only): the clip is a rigid page hinged on the left edge that
// starts edge-on and lays itself down across the frame with true
// perspective foreshortening, shading darker while tilted. Unlike
// 翻页 (a soft paper unroll) this reads as one stiff board.
class BookFlipEffect : public RasterEffect {
public:
    BookFlipEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mDist;
    qsptr<QrealAnimator> mShade;
};

#endif // BOOKFLIPEFFECT_H
