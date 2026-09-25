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
// but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#ifndef PAGEFLIPEFFECT_H
#define PAGEFLIPEFFECT_H

#include "rastereffect.h"

class QrealAnimator;

// CapCut-style page flip transition: over the head window the clip
// unrolls diagonally from the top-left corner like a page being
// laid down, with a shaded curl band riding the leading edge; the
// tail window rolls the page back up on remove. Fully open the band
// has left the frame entirely (plain copy).
class PageFlipEffect : public RasterEffect {
public:
    PageFlipEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mBandWidth;
};

#endif // PAGEFLIPEFFECT_H
