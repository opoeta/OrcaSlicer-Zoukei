#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "libslic3r/ColorMixEngine.hpp"

using namespace Slic3r::ColorMix;

static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

TEST_CASE("CIEDE2000 matches Sharma, Wu & Dalal's reference pairs", "[ColorMix]")
{
    struct Pair { Lab a, b; double de; };
    const Pair pairs[] = {
        {{50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}, 2.0425},
        {{50.0, 3.1571, -77.2803}, {50.0, 0.0, -82.7485}, 2.8615},
        {{50.0, 2.8361, -74.0200}, {50.0, 0.0, -82.7485}, 3.4412},
        {{50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}, 2.3669},
        {{50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}, 27.1492},
        {{50.0, 2.5, 0.0}, {61.0, -5.0, 29.0}, 22.8977},
        {{50.0, 2.5, 0.0}, {56.0, -27.0, -3.0}, 31.9030},
        {{50.0, 2.5, 0.0}, {58.0, 24.0, 15.0}, 19.4535},
        {{60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}, 1.2644},
        {{2.0776, 0.0795, -1.1350}, {0.9033, -0.0636, -0.5514}, 0.9082},
    };
    for (const Pair &p : pairs) {
        CHECK(near(ciede2000(p.a, p.b), p.de, 1e-4));
        CHECK(near(ciede2000(p.b, p.a), p.de, 1e-4));
    }
}

TEST_CASE("Colour space conversions", "[ColorMix]")
{
    RGB c;
    REQUIRE(rgb_from_hex("#FF8000", c));
    REQUIRE(rgb_to_hex(c) == "#FF8000");
    REQUIRE(!rgb_from_hex("#12", c));

    const Lab white = oklab_from_rgb({1, 1, 1});
    CHECK(near(white.L, 1.0, 1e-4));
    CHECK(near(white.a, 0.0, 1e-4));
    CHECK(near(white.b, 0.0, 1e-4));

    const RGB back = rgb_from_oklab(oklab_from_rgb({0.2, 0.6, 0.9}));
    CHECK(near(back.r, 0.2, 1e-6));
    CHECK(near(back.g, 0.6, 1e-6));
    CHECK(near(back.b, 0.9, 1e-6));

    const Lab red = cielab_from_rgb({1, 0, 0});
    CHECK(near(red.L, 53.24, 0.05));
    CHECK(near(red.a, 80.09, 0.05));
    CHECK(near(red.b, 67.20, 0.05));
}

TEST_CASE("Recipes normalise, spread their pattern and round percentages", "[ColorMix]")
{
    const Recipe r{{3, 1}, {4, 2}};
    const Recipe n = r.normalized();
    REQUIRE(n.components == std::vector<int>{1, 3});
    REQUIRE(n.weights == std::vector<int>{1, 2});
    REQUIRE(r == Recipe({{1, 3}, {2, 4}}));

    const std::string pat = Recipe{{0, 1}, {3, 1}}.pattern();
    REQUIRE(pat.size() == 4);
    REQUIRE(std::count(pat.begin(), pat.end(), 'A') == 3);
    REQUIRE(pat.find("AAA") == std::string::npos); // spread, not run-length

    REQUIRE(Recipe({{0, 1}, {1, 2}}).percentages() == std::vector<int>{33, 67});
    REQUIRE(Recipe({{0, 1, 2}, {1, 1, 1}}).percentages() == std::vector<int>{34, 33, 33});
}

TEST_CASE("Prediction stays between its components and honours translucency", "[ColorMix]")
{
    std::vector<Filament> pal(2);
    pal[0].color = {1, 1, 1};
    pal[1].color = {0, 0, 0.8};
    for (Model m : {Model::Pigment, Model::Optical}) {
        ModelParams p;
        p.model = m;
        const RGB half  = predict(pal, Recipe{{0, 1}, {1, 1}}, p);
        const RGB mostw = predict(pal, Recipe{{0, 1}, {3, 1}}, p);
        CHECK(oklab_from_rgb(mostw).L > oklab_from_rgb(half).L);
        CHECK(delta_e(predict(pal, Recipe{{1}, {1}}, p), pal[1].color) < 1e-9);
    }
    // A clear filament barely shows: the mix stays close to the opaque colour.
    ModelParams p;
    p.model = Model::Optical;
    std::vector<Filament> clear = pal;
    clear[0].opacity = 0.05;
    const RGB with_clear = predict(clear, Recipe{{0, 1}, {1, 1}}, p);
    const RGB with_white = predict(pal, Recipe{{0, 1}, {1, 1}}, p);
    CHECK(delta_e(with_clear, pal[1].color) < delta_e(with_white, pal[1].color));
    // TD drives opacity when known.
    Filament f;
    f.td_mm = 2.0;
    CHECK(stripe_opacity(f, 0.2) < stripe_opacity(f, 0.4));
    CHECK(stripe_opacity(f, 2.0) > 0.94);

    // Measured blends win over the model.
    p.measured = [](const std::vector<RGB> &, const std::vector<int> &w, RGB &out) {
        out = {0.25, 0.5, 0.75};
        return w.size() == 2 && w[0] == 50;
    };
    CHECK(rgb_to_hex(predict(pal, Recipe{{0, 1}, {1, 1}}, p)) == rgb_to_hex({0.25, 0.5, 0.75}));
}

TEST_CASE("k-means in OKLab finds weighted groups", "[ColorMix]")
{
    std::vector<RGB>    colors;
    std::vector<double> weights;
    for (int i = 0; i < 5; ++i) {
        colors.push_back({0.9 + 0.01 * i, 0.1, 0.1});
        colors.push_back({0.1, 0.8 + 0.01 * i, 0.1});
        colors.push_back({0.1, 0.1, 0.85 + 0.01 * i});
        weights.insert(weights.end(), {1.0, 2.0, 3.0});
    }
    std::vector<RGB>    centers;
    std::vector<double> cw;
    const auto label = kmeans_oklab(colors, weights, 3, centers, cw);
    REQUIRE(centers.size() == 3);
    for (size_t i = 0; i < colors.size(); i += 3) {
        CHECK(label[i] == label[0]);
        CHECK(label[i + 1] == label[1]);
        CHECK(label[i + 2] == label[2]);
    }
    CHECK(std::set<int>(label.begin(), label.end()).size() == 3);
    CHECK(near(cw[size_t(label[2])], 15.0, 1e-9));

    // k larger than the distinct colours.
    kmeans_oklab({{1, 0, 0}, {1, 0, 0}}, {}, 8, centers, cw);
    CHECK(centers.size() == 1);
}

static std::vector<Filament> cmyw()
{
    std::vector<Filament> pal(4);
    rgb_from_hex("#0086D6", pal[0].color); // cyan
    rgb_from_hex("#EC008C", pal[1].color); // magenta
    rgb_from_hex("#F4EE2A", pal[2].color); // yellow
    rgb_from_hex("#FFFFFF", pal[3].color); // white
    return pal;
}

TEST_CASE("Solver recovers exact recipes and reuses mixes", "[ColorMix]")
{
    const auto    pal = cmyw();
    SolverOptions opt;
    opt.max_colors = 0;
    opt.max_mixes  = 4;
    const Recipe green{{0, 2}, {1, 1}}, pink{{1, 3}, {1, 3}};
    const RGB    tg = predict(pal, green, opt.model), tp = predict(pal, pink, opt.model);
    const Solution sol = solve({tg, tp, tg, pal[1].color}, {1, 1, 1, 1}, pal, opt);
    REQUIRE(sol.clusters.size() == 4);
    CHECK(sol.candidates[size_t(sol.clusters[0].candidate)].recipe == green);
    CHECK(sol.candidates[size_t(sol.clusters[1].candidate)].recipe == pink);
    CHECK(sol.clusters[2].candidate == sol.clusters[0].candidate);
    CHECK(sol.candidates[size_t(sol.clusters[3].candidate)].recipe.is_physical());
    CHECK(sol.new_mixes.size() == 2);
    CHECK(sol.max_delta_e < 0.5);
    CHECK(sol.mean_delta_e < sol.physical_mean_delta_e);
}

TEST_CASE("Solver honours the mix limit, the physical-only switch and existing mixes", "[ColorMix]")
{
    const auto       pal = cmyw();
    SolverOptions    opt;
    opt.max_colors = 0;
    std::vector<RGB> targets;
    for (const Recipe &r : {Recipe{{0, 1}, {1, 1}}, Recipe{{0, 2}, {1, 1}}, Recipe{{1, 2}, {1, 1}}, Recipe{{0, 3}, {1, 3}},
                            Recipe{{1, 3}, {1, 3}}, Recipe{{2, 3}, {1, 3}}})
        targets.push_back(predict(pal, r, opt.model));
    const std::vector<double> w(targets.size(), 1.0);

    opt.max_mixes             = 6;
    const Solution free_sol   = solve(targets, w, pal, opt);
    opt.max_mixes             = 2;
    const Solution limited    = solve(targets, w, pal, opt);
    CHECK(free_sol.new_mixes.size() == 6);
    CHECK(limited.new_mixes.size() <= 2);
    CHECK(limited.mean_delta_e >= free_sol.mean_delta_e);
    CHECK(limited.mean_delta_e <= limited.physical_mean_delta_e);

    opt.only_physical = true;
    const Solution phys = solve(targets, w, pal, opt);
    CHECK(phys.new_mixes.empty());
    for (const Assignment &a : phys.clusters)
        CHECK(phys.candidates[size_t(a.candidate)].recipe.is_physical());
    CHECK(near(phys.mean_delta_e, phys.physical_mean_delta_e, 1e-9));

    opt.only_physical  = false;
    opt.max_mixes      = 0;
    opt.existing_mixes = {Recipe{{0, 2}, {1, 1}}};
    const Solution reuse = solve(targets, w, pal, opt);
    CHECK(reuse.new_mixes.empty());
    CHECK(reuse.candidates[size_t(reuse.clusters[1].candidate)].existing);
    CHECK(reuse.clusters[1].delta_e < 0.5);

    // Triples only appear when allowed.
    opt.existing_mixes.clear();
    opt.max_mixes = 6;
    for (const Candidate &c : enumerate_candidates(pal, opt))
        CHECK(c.recipe.components.size() <= 2);
    opt.max_components = 3;
    bool has_triple = false;
    for (const Candidate &c : enumerate_candidates(pal, opt))
        has_triple = has_triple || c.recipe.components.size() == 3;
    CHECK(has_triple);
}

TEST_CASE("Method choice follows the printer", "[ColorMix]")
{
    CHECK(choose_method({1, true}) == Method::AlternatingLayers);
    CHECK(choose_method({4, true}) == Method::AlternatingLayers);
    CHECK(choose_method({4, false}) == Method::SubLayers);
    CHECK(method_from_key(method_key(Method::SubLayers)) == Method::SubLayers);
    CHECK(model_from_key(model_key(Model::Optical)) == Model::Optical);
}
