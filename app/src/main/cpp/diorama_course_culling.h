#pragma once

namespace gdx {

// The diorama exposes the streamed course from angles and distances that do not match F-Zero's
// native chase-camera depth test. G-Diffuser documents 200% as the effective maximum useful course
// draw distance: beyond that, Course_SegmentsInit has no additional streamed chunks to reveal.
constexpr float AbsFloat(float value) {
    return value < 0.0f ? -value : value;
}

constexpr bool DioramaCourseDepthVisible(float depth, float farDistance) {
    const float effectiveFar = (farDistance * 2.0f) > 1.0f ? (farDistance * 2.0f) : 1.0f;
    return AbsFloat(depth) <= effectiveFar;
}

} // namespace gdx
