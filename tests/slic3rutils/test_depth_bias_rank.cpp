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

// The slicer gives an overlap of two model parts to the later one, so an earlier part must lie behind every later part
// it touches; parts that touch nothing later need no bias.
TEST_CASE("A base gets a depth bias over the inlays set into its bed face", "[DepthBias]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *base   = object->add_volume(make_cube(100., 100., 10.));
    // four inlays sharing the base's bottom face, apart from each other, as an SVG multicolour import makes them
    std::vector<ModelVolume*> inlays;
    for (int i = 0; i < 4; ++i) {
        inlays.push_back(object->add_volume(make_cube(10., 10., 0.4)));
        inlays.back()->set_offset(Vec3d(5. + 20. * i, 5., 0.2)); // centre; x 0..10, 20..30, ...
    }
    object->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);

    CHECK(model_part_depth_bias_rank(*base) == 1);
    for (const ModelVolume *v : inlays)
        CHECK(model_part_depth_bias_rank(*v) == 0);
    for (const ModelVolume *v : object->volumes)
        if (! v->is_model_part())
            CHECK(model_part_depth_bias_rank(*v) == 0);
}

TEST_CASE("Overlapping parts are ordered as the slicer clips them", "[DepthBias]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *a      = object->add_volume(make_cube(10., 10., 1.));
    ModelVolume *b      = object->add_volume(make_cube(10., 10., 1.));
    ModelVolume *c      = object->add_volume(make_cube(10., 10., 1.));
    // add_volume() centres each mesh and keeps its centre as the offset: a spans x 0..10, b 7..17, c 14..24
    a->set_offset(Vec3d(5., 5., 0.5));
    b->set_offset(Vec3d(12., 5., 0.5));
    c->set_offset(Vec3d(19., 5., 0.5)); // touches b, not a
    CHECK(model_part_depth_bias_rank(*a) == 2);
    CHECK(model_part_depth_bias_rank(*b) == 1);
    CHECK(model_part_depth_bias_rank(*c) == 0);
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
    for (int i = 0; i < 30; ++i)
        object->add_volume(make_cube(10., 10., 10.));
    CHECK(model_part_depth_bias_rank(*first) == 16);
}
