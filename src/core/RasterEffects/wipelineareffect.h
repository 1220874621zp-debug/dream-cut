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

#ifndef WIPELINEAREFFECT_H
#define WIPELINEAREFFECT_H

#include "rastereffect.h"

class QrealAnimator;
class ComboBoxProperty;

// CapCut-style linear wipe transition: over the head window a hard
// or soft straight edge sweeps across the clip (from the left /
// right / top / bottom edge, chosen by direction), growing the
// clip's alpha from nothing to fully open. The tail window sweeps
// the edge back out on remove.
class WipeLinearEffect : public RasterEffect {
public:
    WipeLinearEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<ComboBoxProperty> mDirection;
    qsptr<QrealAnimator> mSoftness;
};

#endif // WIPELINEAREFFECT_H
