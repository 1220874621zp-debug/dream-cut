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

#ifndef ZOOMTRANSITIONEFFECT_H
#define ZOOMTRANSITIONEFFECT_H

#include "rastereffect.h"

class QrealAnimator;
class ComboBoxProperty;

// CapCut-style zoom transition: over the head window the clip scales
// in from a magnified (shrink-in) or tiny (grow-in) state centred on
// the frame while fading from transparent to opaque; the tail window
// scales back out on remove.
class ZoomTransitionEffect : public RasterEffect {
public:
    ZoomTransitionEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
    qsptr<QrealAnimator> mAmount;
    qsptr<ComboBoxProperty> mMode;
};

#endif // ZOOMTRANSITIONEFFECT_H
