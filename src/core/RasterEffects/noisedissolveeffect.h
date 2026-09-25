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

#ifndef NOISEDISSOLVEEFFECT_H
#define NOISEDISSOLVEEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style noise dissolve transition: every grain cell (a small
// block of pixels) carries a fixed random threshold; over the head
// window cells whose threshold falls below the growing openness
// become visible one by one, dissolving the clip in from static
// noise. The tail window dissolves back out on remove.
class NoiseDissolveEffect : public RasterEffect {
public:
    NoiseDissolveEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mGrain;
    qsptr<QrealAnimator> mSoftness;
};

#endif // NOISEDISSOLVEEFFECT_H
