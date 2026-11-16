/* Keep the upstream implementation in its own translation unit, as its build
 * does. These wrappers expose the native CPU sampling helpers to the test. */
#include "color_helpers.cpp"

glm::vec3 anland_test_native_luts(const lut1d_t &shaper, const lut3d_t &lut, glm::vec3 value)
{
    return ApplyLut3D_Tetrahedral(lut, ApplyLut1D_Linear(shaper, value));
}
