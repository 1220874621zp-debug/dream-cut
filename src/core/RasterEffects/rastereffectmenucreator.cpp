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

#include "rastereffectmenucreator.h"

#include "RasterEffects/rastereffectsinclude.h"
#include "RasterEffects/customrastereffectcreator.h"
#include "ShaderEffects/shadereffectcreator.h"
#include "ShaderEffects/shadereffect.h"

void RasterEffectMenuCreator::forEveryEffect(const EffectAdder& add) {

    forEveryEffectCore(add);
    forEveryEffectCustom(add);
    forEveryEffectShader(add);
}

void RasterEffectMenuCreator::forEveryEffectCore(const EffectAdder &add)
{
    add(QObject::tr("Blur"), "", []() { return enve::make_shared<BlurEffect>(); });
    add(QObject::tr("Directional Blur"), QObject::tr("Blur"),
        []() { return enve::make_shared<DirectionalBlurEffect>(); });
    add(QObject::tr("Radial Blur"), QObject::tr("Blur"),
        []() { return enve::make_shared<RadialBlurEffect>(); });
    add(QObject::tr("Channel Blur"), QObject::tr("Blur"),
        []() { return enve::make_shared<ChannelBlurEffect>(); });
    add(QObject::tr("Zoom Blur"), QObject::tr("Blur"),
        []() { return enve::make_shared<ZoomBlurEffect>(); });
    add(QObject::tr("Shadow"), "", []() { return enve::make_shared<ShadowEffect>(); });
    add(QObject::tr("Drop Shadow"), QObject::tr("Light"),
        []() { return enve::make_shared<DropShadowEffect>(); });
    add(QObject::tr("Brightness-Contrast"), QObject::tr("Color"),
        []() { return enve::make_shared<BrightnessContrastEffect>(); });
    add(QObject::tr("Colorize"), QObject::tr("Color"),
        []() { return enve::make_shared<ColorizeEffect>(); });
    add(QObject::tr("Color Grading"), QObject::tr("Color"),
        []() { return enve::make_shared<ColorGradingEffect>(); });
    add(QObject::tr("Invert"), QObject::tr("Color"),
        []() { return enve::make_shared<InvertEffect>(); });
    add(QObject::tr("Tint"), QObject::tr("Color"),
        []() { return enve::make_shared<TintEffect>(); });
    add(QObject::tr("Posterize"), QObject::tr("Color"),
        []() { return enve::make_shared<PosterizeEffect>(); });
    add(QObject::tr("Chroma Key"), QObject::tr("Color"),
        []() { return enve::make_shared<ChromaKeyEffect>(); });
    add(QObject::tr("Glow"), QObject::tr("Light"),
        []() { return enve::make_shared<GlowEffect>(); });
    add(QObject::tr("Chromatic Aberration"), QObject::tr("Distort"),
        []() { return enve::make_shared<ChromaticAberrationEffect>(); });
    add(QObject::tr("Mirror"), QObject::tr("Distort"),
        []() { return enve::make_shared<MirrorEffect>(); });
    add(QObject::tr("Twirl"), QObject::tr("Distort"),
        []() { return enve::make_shared<TwirlEffect>(); });
    add(QObject::tr("Shake"), QObject::tr("Distort"),
        []() { return enve::make_shared<ShakeEffect>(); });
    add(QObject::tr("Vignette"), QObject::tr("Stylize"),
        []() { return enve::make_shared<VignetteEffect>(); });
    add(QObject::tr("Letterbox"), QObject::tr("Stylize"),
        []() { return enve::make_shared<LetterboxEffect>(); });
    add(QObject::tr("Scanlines"), QObject::tr("Stylize"),
        []() { return enve::make_shared<ScanlinesEffect>(); });
    add(QObject::tr("Edge Detect"), QObject::tr("Stylize"),
        []() { return enve::make_shared<EdgeDetectEffect>(); });
    add(QObject::tr("Pixelate"), QObject::tr("Stylize"),
        []() { return enve::make_shared<PixelateEffect>(); });
    add(QObject::tr("Noise"), QObject::tr("Stylize"),
        []() { return enve::make_shared<NoiseEffect>(); });
    add(QObject::tr("Glitch"), QObject::tr("Stylize"),
        []() { return enve::make_shared<GlitchEffect>(); });
    add(QObject::tr("Halftone"), QObject::tr("Stylize"),
        []() { return enve::make_shared<HalftoneEffect>(); });
    add(QObject::tr("Light Sweep"), QObject::tr("Light"),
        []() { return enve::make_shared<LightSweepEffect>(); });
    add(QObject::tr("Film Grain"), QObject::tr("Stylize"),
        []() { return enve::make_shared<FilmGrainEffect>(); });
    add(QObject::tr("Black-White Flash"), QObject::tr("Stylize"),
        []() { return enve::make_shared<BlackWhiteFlashEffect>(); });
    add(QObject::tr("Liquid Glass"), QObject::tr("Distort"),
        []() { return enve::make_shared<LiquidGlassEffect>(); });
    add(QObject::tr("Pixel Art"), QObject::tr("Stylize"),
        []() { return enve::make_shared<PixelArtEffect>(); });
    // Wipe/Stripe/Noise Fade 是 8ec62fe61 时代的旧单块特效（挂层上
    // 对本块做视觉处理），不是块间转场——留在 Transitions 类目会让
    // 过渡面板出现"点了只加个普通特效"的卡片：无重叠模型、无转场
    // 条 UI、无跨块过渡（用户感知=有些转场没 UI/没效果）。归入
    // Stylize，过渡类目只留真转场
    add(QObject::tr("Wipe"), QObject::tr("Stylize"),
        []() { return enve::make_shared<WipeEffect>(); });
    add(QObject::tr("Stripe"), QObject::tr("Stylize"),
        []() { return enve::make_shared<StripeEffect>(); });
    add(QObject::tr("Noise Fade"), QObject::tr("Stylize"),
        []() { return enve::make_shared<NoiseFadeEffect>(); });
    add(QObject::tr("叠化（淡入淡出）"), QObject::tr("Transitions"),
        []() { return enve::make_shared<DissolveEffect>(); });
    add(QObject::tr("闪黑"), QObject::tr("Transitions"),
        []() { return enve::make_shared<FlashFadeEffect>(Qt::black); });
    add(QObject::tr("闪白"), QObject::tr("Transitions"),
        []() { return enve::make_shared<FlashFadeEffect>(Qt::white); });
    add(QObject::tr("滑动"), QObject::tr("Transitions"),
        []() { return enve::make_shared<SlideEffect>(); });
    add(QObject::tr("圆形划像"), QObject::tr("Transitions"),
        []() { return enve::make_shared<WipeCircleEffect>(); });
    // second wave: legacy Wipe/Stripe/NoiseFade rebuilt as real
    // transitions + blur/zoom/mosaic variants from other categories
    add(QObject::tr("线性划像"), QObject::tr("Transitions"),
        []() { return enve::make_shared<WipeLinearEffect>(); });
    add(QObject::tr("百叶窗"), QObject::tr("Transitions"),
        []() { return enve::make_shared<BlindsEffect>(); });
    add(QObject::tr("噪波渐变"), QObject::tr("Transitions"),
        []() { return enve::make_shared<NoiseDissolveEffect>(); });
    add(QObject::tr("模糊叠化"), QObject::tr("Transitions"),
        []() { return enve::make_shared<BlurDissolveEffect>(); });
    add(QObject::tr("缩放"), QObject::tr("Transitions"),
        []() { return enve::make_shared<ZoomTransitionEffect>(); });
    add(QObject::tr("马赛克"), QObject::tr("Transitions"),
        []() { return enve::make_shared<MosaicDissolveEffect>(); });
    add(QObject::tr("旋转"), QObject::tr("Transitions"),
        []() { return enve::make_shared<SpinEffect>(); });
    add(QObject::tr("镜像翻转"), QObject::tr("Transitions"),
        []() { return enve::make_shared<MirrorFlipEffect>(); });
    add(QObject::tr("漩涡"), QObject::tr("Transitions"),
        []() { return enve::make_shared<TwirlTransitionEffect>(); });
    add(QObject::tr("故障"), QObject::tr("Transitions"),
        []() { return enve::make_shared<GlitchTransitionEffect>(); });
    add(QObject::tr("抖动"), QObject::tr("Transitions"),
        []() { return enve::make_shared<ShakeTransitionEffect>(); });
    add(QObject::tr("变焦模糊"), QObject::tr("Transitions"),
        []() { return enve::make_shared<ZoomBlurTransitionEffect>(); });
    add(QObject::tr("方向模糊"), QObject::tr("Transitions"),
        []() { return enve::make_shared<DirectionalBlurTransitionEffect>(); });
    add(QObject::tr("通道分离"), QObject::tr("Transitions"),
        []() { return enve::make_shared<ChannelSplitTransitionEffect>(); });
    add(QObject::tr("半调网点"), QObject::tr("Transitions"),
        []() { return enve::make_shared<HalftoneTransitionEffect>(); });
    add(QObject::tr("线稿"), QObject::tr("Transitions"),
        []() { return enve::make_shared<EdgeTransitionEffect>(); });
    add(QObject::tr("反色"), QObject::tr("Transitions"),
        []() { return enve::make_shared<InvertTransitionEffect>(); });
    add(QObject::tr("海报"), QObject::tr("Transitions"),
        []() { return enve::make_shared<PosterizeTransitionEffect>(); });
    add(QObject::tr("暗角"), QObject::tr("Transitions"),
        []() { return enve::make_shared<VignetteTransitionEffect>(); });
    add(QObject::tr("扫光"), QObject::tr("Transitions"),
        []() { return enve::make_shared<LightSweepTransitionEffect>(); });
    add(QObject::tr("胶片颗粒"), QObject::tr("Transitions"),
        []() { return enve::make_shared<FilmGrainTransitionEffect>(); });
    add(QObject::tr("翻页"), QObject::tr("Transitions"),
        []() { return enve::make_shared<PageFlipEffect>(); });
    add(QObject::tr("卷页 (Page Curl)"), QObject::tr("Distort"),
        []() { return enve::make_shared<PageCurlEffect>(); });
    add(QObject::tr("晶格变形"), QObject::tr("Distort"),
        []() { return enve::make_shared<LatticeWarpEffect>(); });
    add(QObject::tr("图层样式"), "",
        []() { return enve::make_shared<LayerStylesEffect>(); });
}

void RasterEffectMenuCreator::forEveryEffectCustom(const EffectAdder &add)
{
    CustomRasterEffectCreator::sForEveryEffect(add);
}

void RasterEffectMenuCreator::forEveryEffectShader(const EffectAdder &add)
{
    ShaderEffectCreator::sForEveryEffect(add);
}
