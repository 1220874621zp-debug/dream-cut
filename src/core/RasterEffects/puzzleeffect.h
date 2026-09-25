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

#ifndef PUZZLEEFFECT_H
#define PUZZLEEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style puzzle slide (gl-transitions "PuzzleRight" port): the
// frame splits into grid pieces that slide in from beyond the right
// edge in a random order with an ease-out settle, a soft seam
// darkening the piece borders while they travel.
class PuzzleEffect : public RasterEffect {
public:
    PuzzleEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mCellSize;
    qsptr<QrealAnimator> mSeam;
};

#endif // PUZZLEEFFECT_H
