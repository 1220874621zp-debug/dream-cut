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

#ifndef FRAGMENTEFFECT_H
#define FRAGMENTEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style fragment assembly (gl-transitions "fragment" port,
// incoming side only): the frame shatters into grid shards that fly
// in from off-screen - each rotated, shrunk and pushed radially away
// from the centre - converging into the intact frame as the head
// window opens.
class FragmentEffect : public RasterEffect {
public:
    FragmentEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mCellSize;
    qsptr<QrealAnimator> mDistance;
    qsptr<QrealAnimator> mSpin;
};

#endif // FRAGMENTEFFECT_H
