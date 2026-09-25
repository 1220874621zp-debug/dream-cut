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
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#ifndef MOSAICDISSOLVEEFFECT_H
#define MOSAICDISSOLVEEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style mosaic transition: over the head window the clip
// appears as a heavy pixel mosaic and resolves to sharp pixels while
// fading in from transparent; the tail window mosaics back out on
// remove. The mosaic grid is anchored to the frame origin so it
// stays put while the block size shrinks.
class MosaicDissolveEffect : public RasterEffect {
public:
    MosaicDissolveEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mMaxBlock;
};

#endif // MOSAICDISSOLVEEFFECT_H
