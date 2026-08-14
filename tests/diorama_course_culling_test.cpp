#include "diorama_course_culling.h"

static_assert(gdx::DioramaCourseDepthVisible(0.0f, 5500.0f));
static_assert(gdx::DioramaCourseDepthVisible(8800.0f, 5500.0f));
static_assert(gdx::DioramaCourseDepthVisible(-8800.0f, 5500.0f));
static_assert(gdx::DioramaCourseDepthVisible(11000.0f, 5500.0f));
static_assert(!gdx::DioramaCourseDepthVisible(11001.0f, 5500.0f));

int main() { return 0; }
