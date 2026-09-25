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
# See 'README.md' for more information.
#
*/

// Fork of enve - Copyright (C) 2016-2020 Maurycy Liebner

#ifndef RASTEREFFECT_H
#define RASTEREFFECT_H
#include "../Animators/eeffect.h"
#include "../glhelpers.h"
#include "rastereffectcaller.h"

// Windows GDI defines these as macros
#ifdef INVERT
#undef INVERT
#endif
#ifdef HALFTONE
#undef HALFTONE
#endif
#ifdef MIRROR
#undef MIRROR
#endif
#ifdef NOISE
#undef NOISE
#endif
#ifdef TINT
#undef TINT
#endif
#ifdef RAIN
#undef RAIN
#endif
#ifdef SHAKE
#undef SHAKE
#endif
#ifdef STRIPE
#undef STRIPE
#endif
#ifdef GLOW
#undef GLOW
#endif
#ifdef CHROMA_KEY
#undef CHROMA_KEY
#endif
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif
#ifdef OPAQUE
#undef OPAQUE
#endif
#ifdef ERROR
#undef ERROR
#endif

enum class RasterEffectType : short {
    BLUR,
    SHADOW,
    CUSTOM, // C++
    CUSTOM_SHADER, // xml, GLSL
    // 4 was MOTION_BLUR (removed 2026-09-23, animator-only: samples
    // the layer motion path, useless for video clips) - reserved so
    // the serialized ids below do not shift against old project files
    WIPE = 5,
    // 6 was BONE_WARP (removed) - kept reserved so the serialized ids
    // of the effects below do not shift against old project files
    NOISE_FADE = 7,
    COLORIZE,
    BRIGHTNESS_CONTRAST,
    CHROMA_KEY,      // 10, as in every build since the chroma-key commit
    LIQUID_GLASS,    // 11, ditto - keep 0..11 stable for saved projects
    // ported AE effect family re-appended after the fork's own slots
    // (merge fix: the rebase dropped these while their .cpps stayed)
    VIGNETTE,
    CHROMATIC_ABERRATION,
    LETTERBOX,
    SCANLINES,
    GLOW,
    DIRECTIONAL_BLUR,
    RADIAL_BLUR,
    // 19 was WAVE_WARP (removed 2026-09-23, animator-style param
    // warp - not a video-editing effect); 20 was RAIN ( removed 2026-09-23, simulation - animator-only)
    EDGE_DETECT = 21,
    INVERT,
    TINT,
    PIXELATE,
    NOISE,
    MIRROR,
    GLITCH,
    POSTERIZE,
    TWIRL,
    CHANNEL_BLUR,
    HALFTONE,
    SHAKE,
    DROP_SHADOW,
    ZOOM_BLUR,
    COLOR_GRADING,
    STRIPE,
    // 37 was MOTION_TILE / 38 was FRACTAL_NOISE (removed 2026-09-23,
    // texture-generation & tiling - animator-only)
    LIGHT_SWEEP = 39,
    // 40 was DISPLACEMENT_WARP (removed 2026-09-23, needs a
    // displacement map - not a video-editing workflow)
    FILM_GRAIN = 41,
    BLACK_WHITE_FLASH,
    PIXEL_ART,
    // Photoshop-style layer styles container (shadow/glow/stroke);
    // appended last, never reorder - serialized ids must stay stable
    LAYER_STYLES,
    // 45 was SHATTER / 46 was SMEAR / 47 was ROUGHEN_EDGES / 48 was
    // PARTICLE (all removed 2026-09-23 - simulation / smear / text
    // fringe styling, animator-only)
    // cylindrical page curl with N.L shading (Foldspace-style roll)
    PAGE_CURL = 49,
    // AE Putty-style lattice (FFD) deformation - appended last,
    // never reorder, serialized ids must stay stable
    LATTICE_WARP,
    // CapCut-style clip transitions (kdenlive ports): dissolve alpha
    // crossfade / flash color fade / slide translation / circle wipe,
    // anchored to the clip head/tail windows
    TRANSITION_DISSOLVE,
    TRANSITION_FLASH,
    TRANSITION_SLIDE,
    TRANSITION_WIPE_CIRCLE,
    // second wave: legacy Wipe/Stripe/NoiseFade rebuilt as real
    // transitions + blur/zoom/mosaic transition variants
    TRANSITION_WIPE_LINEAR,
    TRANSITION_BLINDS,
    TRANSITION_NOISE,
    TRANSITION_BLUR,
    TRANSITION_ZOOM,
    TRANSITION_MOSAIC,
    // third wave: every remaining effect with a well-defined
    // "unfold into place" meaning rebuilt as a transition
    TRANSITION_SPIN,
    TRANSITION_MIRROR_FLIP,
    TRANSITION_TWIRL,
    TRANSITION_GLITCH,
    TRANSITION_SHAKE,
    TRANSITION_ZOOM_BLUR,
    TRANSITION_DIR_BLUR,
    TRANSITION_CHANNEL_SPLIT,
    TRANSITION_HALFTONE,
    TRANSITION_EDGE,
    TRANSITION_INVERT,
    TRANSITION_POSTERIZE,
    TRANSITION_VIGNETTE,
    TRANSITION_LIGHT_SWEEP,
    TRANSITION_FILM_GRAIN,
    TRANSITION_PAGE_FLIP
};

struct BoxRenderData;
class ComboBoxProperty;

// single source of truth for "is this a real clip transition": the
// model/panel checks and the base-class ease parameter all route
// through here (app layers must not keep their own copies)
inline bool isTransitionEffectType(const RasterEffectType t)
{
    switch (t) {
    case RasterEffectType::TRANSITION_DISSOLVE:
    case RasterEffectType::TRANSITION_FLASH:
    case RasterEffectType::TRANSITION_SLIDE:
    case RasterEffectType::TRANSITION_WIPE_CIRCLE:
    case RasterEffectType::TRANSITION_WIPE_LINEAR:
    case RasterEffectType::TRANSITION_BLINDS:
    case RasterEffectType::TRANSITION_NOISE:
    case RasterEffectType::TRANSITION_BLUR:
    case RasterEffectType::TRANSITION_ZOOM:
    case RasterEffectType::TRANSITION_MOSAIC:
    case RasterEffectType::TRANSITION_SPIN:
    case RasterEffectType::TRANSITION_MIRROR_FLIP:
    case RasterEffectType::TRANSITION_TWIRL:
    case RasterEffectType::TRANSITION_GLITCH:
    case RasterEffectType::TRANSITION_SHAKE:
    case RasterEffectType::TRANSITION_ZOOM_BLUR:
    case RasterEffectType::TRANSITION_DIR_BLUR:
    case RasterEffectType::TRANSITION_CHANNEL_SPLIT:
    case RasterEffectType::TRANSITION_HALFTONE:
    case RasterEffectType::TRANSITION_EDGE:
    case RasterEffectType::TRANSITION_INVERT:
    case RasterEffectType::TRANSITION_POSTERIZE:
    case RasterEffectType::TRANSITION_VIGNETTE:
    case RasterEffectType::TRANSITION_LIGHT_SWEEP:
    case RasterEffectType::TRANSITION_FILM_GRAIN:
    case RasterEffectType::TRANSITION_PAGE_FLIP:
        return true;
    default:
        return false;
    }
}

class CORE_EXPORT RasterEffect : public eEffect {
    e_OBJECT
    Q_OBJECT
protected:
    RasterEffect(const QString &name,
                 const HardwareSupport hwSupport,
                 const bool hwInterchangeable,
                 const RasterEffectType type);
public:
    virtual stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame,
            const qreal resolution,
            const qreal influence,
            BoxRenderData * const data) const = 0;

    virtual bool forceMargin() const { return false; }
    virtual QMargins getMargin() const { return QMargins(); }

    QMimeData *SWT_createMimeData() final;

    void prp_setupTreeViewMenu(PropertyMenu * const menu);

    void writeIdentifier(eWriteStream& dst) const;
    void writeIdentifierXEV(QDomElement& ele) const;

    // AE-style reset: rebuild a factory-default instance of this
    // effect's type and copy its parameter values over (undoable)
    void resetToDefault();

    RasterEffectType getEffectType() const {
        return mType;
    }

    HardwareSupport instanceHwSupport() const {
        return mInstHwSupport;
    }

    void switchInstanceHwSupport();

    // transition ease curve carried by every real transition (the
    // base class attaches the "缓动" combo in the constructor);
    // 0 linear, see nleTransitionEase in transitionanchor.h.
    // transitionEaseProperty exposes the combo itself so property
    // panels can render a row for it (it is NOT a ca child, see the
    // serialization note in the .cpp)
    int transitionEaseMode() const;
    ComboBoxProperty *transitionEaseProperty() const
    { return mEase.data(); }

    // legacy child order + visible + name, then (transitions only)
    // the ease value behind the EvFormat::transitionEase gate
    void prp_writeProperty_impl(eWriteStream &dst) const;
    void prp_readProperty_impl(eReadStream &src);
signals:
    void hardwareSupportChanged();
    void forcedMarginChanged();
private:
    const RasterEffectType mType;
    const HardwareSupport mTypeHwSupport;
    const bool mHwInterchangeable;
    HardwareSupport mInstHwSupport;
    qsptr<ComboBoxProperty> mEase;
};

#endif // RASTEREFFECT_H
