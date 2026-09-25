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
*/

#ifndef STARWIPEEFFECT_H
#define STARWIPEEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style star reveal (gl-transitions "StarWipe" port): an
// N-point star grows from the centre, its spikes rushing off-canvas
// before the valleys between them cover the corners; fully open the
// whole frame sits inside the valleys (plain copy).
class StarWipeEffect : public RasterEffect {
public:
    StarWipeEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mPoints;
    qsptr<QrealAnimator> mInner;
    qsptr<QrealAnimator> mSoftness;
};

#endif // STARWIPEEFFECT_H
