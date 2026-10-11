#include "ModernGlassBackdrop.h"

#include <cmath>
#include <array>
#include <vector>

namespace singleeditor
{
juce::Image ModernGlassBackdrop::blur(const juce::Image& source, float radius, juce::Colour background)
{
    if (!source.isValid() || source.getWidth() > 1024 || source.getHeight() > 512 || !std::isfinite(radius)) return {};
    juce::Image result(juce::Image::RGB, source.getWidth(), source.getHeight(), false, juce::SoftwareImageType());
    {
        juce::Graphics g(result);
        g.fillAll(background);
        g.drawImageAt(source, 0, 0);
    }
    // The same finite Gaussian kernel as JUCE's software 2D convolution,
    // factored into horizontal/vertical passes. Keep its even-kernel centre
    // and zero-outside boundary rule so the sampled glass does not shift.
    radius = std::clamp(radius, 0.5f, 8.0f);
    const int kernelSize = juce::roundToInt(radius * 2.0f), centre = kernelSize / 2;
    std::array<float, 16> weights {};
    double total = 0.0;
    for (int i = 0; i < kernelSize; ++i)
    {
        const double distance = i - centre;
        weights[static_cast<size_t>(i)] = static_cast<float>(std::exp(-distance * distance / (2.0 * radius * radius)));
        total += weights[static_cast<size_t>(i)];
    }
    for (auto& weight : weights) weight = static_cast<float>(weight / total);
    const int width = result.getWidth(), height = result.getHeight();
    // At the existing 1024x512 cap this is at most 6 MiB, temporary per capture.
    // Float intermediates avoid two separate byte-rounding passes.
    std::vector<float> horizontal(static_cast<size_t>(width) * height * 3);
    {
        const juce::Image::BitmapData pixels(result, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const int first = std::max(0, centre - x), end = std::min(kernelSize, width + centre - x);
                const auto* pixel = pixels.getPixelPointer(x + first - centre, y);
                float a = 0, b = 0, c = 0;
                for (int k = first; k < end; ++k, pixel += 3)
                {
                    const auto weight = weights[static_cast<size_t>(k)];
                    a += pixel[0] * weight; b += pixel[1] * weight; c += pixel[2] * weight;
                }
                auto* output = horizontal.data() + (static_cast<size_t>(y) * width + x) * 3;
                output[0] = a; output[1] = b; output[2] = c;
            }
    }
    {
        const juce::Image::BitmapData pixels(result, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y)
        {
            const int first = std::max(0, centre - y), end = std::min(kernelSize, height + centre - y);
            for (int x = 0; x < width; ++x)
            {
                float a = 0, b = 0, c = 0;
                for (int k = first; k < end; ++k)
                {
                    const auto* pixel = horizontal.data() + (static_cast<size_t>(y + k - centre) * width + x) * 3;
                    const auto weight = weights[static_cast<size_t>(k)];
                    a += pixel[0] * weight; b += pixel[1] * weight; c += pixel[2] * weight;
                }
                auto* output = pixels.getPixelPointer(x, y);
                output[0] = static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(a)));
                output[1] = static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(b)));
                output[2] = static_cast<juce::uint8>(juce::jlimit(0, 255, juce::roundToInt(c)));
            }
        }
    }
    return result;
}

juce::Point<float> ModernGlassBackdrop::refractedPoint(juce::Point<float> point,
    juce::Rectangle<float> body, float cornerRadius, float strength) noexcept
{
    if (body.isEmpty()) return point;
    const auto p = point - body.getCentre();
    const auto r = juce::jmax(0.25f, juce::jmin(juce::jmin(body.getWidth(), body.getHeight()) * 0.5f, cornerRadius));
    const auto qx = std::abs(p.x) - body.getWidth() * 0.5f + r;
    const auto qy = std::abs(p.y) - body.getHeight() * 0.5f + r;
    const auto ox = juce::jmax(0.0f, qx), oy = juce::jmax(0.0f, qy);
    const auto length = std::hypot(ox, oy);
    const auto distance = -(length + juce::jmin(juce::jmax(qx, qy), 0.0f) - r);
    const auto band = juce::jmax(2.0f, r * 0.9f);
    if (distance <= 0 || distance >= band) return point;
    juce::Point<float> normal;
    if (length > 0.001f)
        normal = { std::copysign(ox / length, p.x), std::copysign(oy / length, p.y) };
    else if (qx > qy) normal.x = std::copysign(1.0f, p.x);
    else normal.y = std::copysign(1.0f, p.y);
    // A curved boundary lens: zero displacement at its outer/inner limits,
    // greatest refraction between them. Centre content remains undistorted.
    const auto displacement = std::sin(distance / band * juce::MathConstants<float>::pi)
        * juce::jmin(3.5f, r * 0.48f) * std::clamp(strength, 0.0f, 1.25f);
    return point + normal * displacement;
}

juce::Image ModernGlassBackdrop::sample(const juce::Image& blurred, juce::Rectangle<int> body,
    float cornerRadius, ModernGlassMaterial material, float strength)
{
    if (!blurred.isValid() || body.isEmpty() || !blurred.getBounds().contains(body)
        || material == ModernGlassMaterial::solid) return {};
    // The prepared backdrop is RGB. Integer-position sampling is an exact copy,
    // so preserve it once instead of interpolating every unchanged interior pixel
    // on each liquid hover tick. Keep the generic conversion for other formats.
    const bool copiedRgb = blurred.getFormat() == juce::Image::RGB;
    juce::Image result = copiedRgb ? blurred.getClippedImage(body).createCopy()
        : juce::Image(juce::Image::RGB, body.getWidth(), body.getHeight(), false, juce::SoftwareImageType());
    if (copiedRgb && material != ModernGlassMaterial::liquid) return result;
    const juce::Image::BitmapData source(blurred, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData target(result, juce::Image::BitmapData::writeOnly);
    const auto local = result.getBounds().toFloat();
    const auto r = juce::jmax(0.25f, juce::jmin(juce::jmin(local.getWidth(), local.getHeight()) * 0.5f, cornerRadius));
    // Inside this conservative rectangle the rounded-boundary distance is at
    // least max(2,r), outside the lens band max(2,0.9*r). Those pixels cannot bend.
    const int inset = static_cast<int>(std::ceil(juce::jmax(2.0f, r)));
    for (int y = 0; y < target.height; ++y)
        for (int x = 0; x < target.width; ++x)
        {
            if (copiedRgb && y >= inset && y < target.height - inset
                && x == inset && x < target.width - inset)
            {
                x = target.width - inset - 1;
                continue;
            }
            auto p = juce::Point<float>(static_cast<float>(x), static_cast<float>(y));
            if (material == ModernGlassMaterial::liquid)
                p = refractedPoint(p, local, cornerRadius, strength);
            p += body.getPosition().toFloat();
            p.x = juce::jlimit(0.0f, static_cast<float>(source.width - 1), p.x);
            p.y = juce::jlimit(0.0f, static_cast<float>(source.height - 1), p.y);
            const auto x0 = static_cast<int>(p.x), y0 = static_cast<int>(p.y);
            const auto x1 = juce::jmin(x0 + 1, source.width - 1), y1 = juce::jmin(y0 + 1, source.height - 1);
            const auto fx = p.x - static_cast<float>(x0), fy = p.y - static_cast<float>(y0);
            const auto top = source.getPixelColour(x0, y0).interpolatedWith(source.getPixelColour(x1, y0), fx);
            const auto bottom = source.getPixelColour(x0, y1).interpolatedWith(source.getPixelColour(x1, y1), fx);
            target.setPixelColour(x, y, top.interpolatedWith(bottom, fy));
        }
    return result;
}
} // namespace singleeditor
