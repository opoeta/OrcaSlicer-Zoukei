// Orca Zoukei: depth-bias rank of model parts in Prepare (3DScene). Pure model logic, no GL context needed.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    // Match the GUI precompiled header: wx/msw/wrapcctl.h needs HDITEM from CommCtrl.h.
    #include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/3DScene.hpp"

using namespace Slic3r;

// The slicer gives an overlap of two model parts to the later one, so every earlier part must be pushed further back
// than every later part: strictly decreasing ranks in volume order, 0 for the last part.
TEST_CASE("Earlier model parts get a larger depth bias than later ones", "[DepthBias]")
{
    Model        model;
    ModelObject *object = model.add_object();
    // a base and four inlays sharing its bottom face, as an SVG multicolour import makes them
    object->add_volume(make_cube(100., 100., 10.));
    for (int i = 0; i < 4; ++i)
        object->add_volume(make_cube(10., 10., 0.4));
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);

    std::vector<int> part_ranks;
    for (const ModelVolume *v : object->volumes) {
        if (v->is_model_part())
            part_ranks.push_back(model_part_depth_bias_rank(*v));
        else
            CHECK(model_part_depth_bias_rank(*v) == 0);
    }
    REQUIRE(part_ranks.size() == 5);
    for (size_t i = 0; i < part_ranks.size(); ++i)
        CHECK(part_ranks[i] == int(part_ranks.size() - 1 - i));
}

TEST_CASE("A single-part object gets no depth bias", "[DepthBias]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *part   = object->add_volume(make_cube(10., 10., 10.));
    CHECK(model_part_depth_bias_rank(*part) == 0);
}

TEST_CASE("The depth bias rank is capped", "[DepthBias]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *first  = object->add_volume(make_cube(10., 10., 10.));
    for (int i = 0; i < 100; ++i)
        object->add_volume(make_cube(1., 1., 1.));
    CHECK(model_part_depth_bias_rank(*first) == 64);
}
