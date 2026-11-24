#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "Base64.hpp"
#include "Calculix.hpp"
#include "Gmsh.hpp"
#include "Mesh.hpp"
#include "Pipeline.hpp"
#include "Request.hpp"
#include "ResultFile.hpp"
#include "test_geometry.hpp"

using namespace Borpa::Sim;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

std::string work_dir(const std::string& name)
{
    std::string dir = make_job_dir(SIM_TEST_WORK, name);
    REQUIRE_FALSE(dir.empty());
    return dir;
}

template<class T> std::string b64(const std::vector<T>& v) { return base64_encode(v.data(), v.size() * sizeof(T)); }

nlohmann::json cantilever_message(const SurfaceGeometry& g)
{
    return {
        { "borpa", 1 },
        { "type", "sim.run" },
        { "job", "job_1" },
        { "study",
          { { "version", 1 },
            { "kind", "static" },
            { "object", "1" },
            { "material", { { "name", "PLA" }, { "E", 2000 }, { "nu", 0.3 }, { "density", 1.24e-9 } } },
            { "fixtures", { { { "faces", { 0 } } } } },
            { "loads", { { { "type", "force" }, { "faces", { 1 } }, { "vector", { 0, 0, -10 } } } } },
            { "mesh", { { "size", nullptr }, { "refine", nlohmann::json::array() } } } } },
        { "geometry",
          { { "objects",
              { { { "id", "1" },
                  { "faces", g.faces },
                  { "positions", b64(g.positions) },
                  { "indices", b64(g.indices) },
                  { "faceIndex", b64(g.face_index) } } } } } },
    };
}

// One tetrahedron (0,0,0) (1,0,0) (0,1,0) (0,0,1) whose four faces are four Hew faces.
const char* k_one_tet_msh = R"($MeshFormat
2.2 0 8
$EndMeshFormat
$Nodes
5
10 0 0 0
11 1 0 0
12 0 1 0
13 0 0 1
99 5 5 5
$EndNodes
$Elements
6
1 15 2 0 1 99
2 2 2 1 1 10 12 11
3 2 2 2 2 10 11 13
4 2 2 3 3 11 12 13
5 2 2 4 4 12 10 13
6 4 2 1 1 10 12 11 13
$EndElements
)";

} // namespace

TEST_CASE("base64 round-trips and rejects bad input", "[sim][format]")
{
    for (std::string s : { "", "a", "ab", "abc", "abcd", "hello world!" }) {
        std::vector<uint8_t> out;
        REQUIRE(base64_decode(base64_encode(s.data(), s.size()), out));
        CHECK(std::string(out.begin(), out.end()) == s);
    }
    CHECK(base64_encode("Man", 3) == "TWFu");
    CHECK(base64_encode("Ma", 2) == "TWE=");
    std::vector<uint8_t> out;
    CHECK_FALSE(base64_decode("TW*u", out));
    CHECK_FALSE(base64_decode("TW=u", out));
}

TEST_CASE("a sim.run message parses into a valid request", "[sim][format]")
{
    SurfaceGeometry g = Test::box(100, 10, 10);
    JobRequest      r;
    std::string     error;
    REQUIRE(parse_job_request(cantilever_message(g).dump(), r, error));
    CHECK(r.job == "job_1");
    CHECK(r.study.material.youngs_modulus == 2000);
    CHECK(r.study.fixtures.size() == 1);
    CHECK(r.study.loads[0].vector[2] == -10);
    CHECK(r.study.mesh_size == 0);
    CHECK(r.geometry.positions == g.positions);
    CHECK(r.geometry.indices == g.indices);
    CHECK(r.geometry.face_index == g.face_index);
}

TEST_CASE("invalid studies are refused with a reason", "[sim][format]")
{
    SurfaceGeometry g = Test::box(100, 10, 10);
    auto            refused = [&](auto edit) {
        nlohmann::json m = cantilever_message(g);
        edit(m);
        JobRequest  r;
        std::string error;
        bool        ok = parse_job_request(m.dump(), r, error);
        CHECK_FALSE(error.empty());
        return !ok;
    };
    CHECK(refused([](nlohmann::json& m) { m["job"] = "../evil"; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["version"] = 2; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["kind"] = "heat"; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["object"] = "2"; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["fixtures"] = nlohmann::json::array(); }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["fixtures"][0]["faces"] = { 6 }; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["loads"][0]["vector"] = { 0, 0, 0 }; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["material"]["nu"] = 0.5; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["material"]["E"] = 0; }));
    CHECK(refused([](nlohmann::json& m) { m["study"]["mesh"]["size"] = -1; }));
    CHECK(refused([](nlohmann::json& m) { m["geometry"]["objects"][0]["positions"] = "not base64!"; }));
    CHECK(refused([](nlohmann::json& m) { m["geometry"]["objects"][0]["faceIndex"] = ""; }));
    CHECK(refused([](nlohmann::json& m) { m["geometry"]["objects"].push_back(m["geometry"]["objects"][0]); }));
    CHECK(refused([](nlohmann::json& m) { m["geometry"] = 3; }));
}

TEST_CASE("automatic mesh size follows the part's size and thickness", "[sim][format]")
{
    CHECK(automatic_mesh_size(Test::box(100, 10, 10)) == Approx(std::sqrt(10200.0) / 40));
    CHECK(automatic_mesh_size(Test::box(100, 20, 5)) == Approx(2.5)); // half the thickness
    // The element budget wins on a large thin plate: cbrt(12 V / 40000) with V = 100,000 mm³.
    CHECK(automatic_mesh_size(Test::box(200, 100, 5)) == Approx(std::cbrt(30.0)));
}

TEST_CASE("the Gmsh input has one surface per face and a refinement field", "[sim][format]")
{
    std::string     dir = work_dir("gmsh_input");
    SurfaceGeometry g   = Test::box(100, 10, 10);
    StaticStudy     s;
    s.refine.push_back({ { 1 }, 0.5 });
    std::string error;
    REQUIRE(write_gmsh_input(dir, g, s, 2.5, error));

    std::ifstream     stl(fs::u8path(dir) / "part.stl");
    std::stringstream stl_text;
    stl_text << stl.rdbuf();
    size_t solids = 0;
    for (size_t p = 0; (p = stl_text.str().find("\nsolid ", p)) != std::string::npos; ++p)
        ++solids;
    CHECK(solids + (stl_text.str().rfind("solid ", 0) == 0) == 6);

    std::ifstream     geo(fs::u8path(dir) / "part.geo");
    std::stringstream geo_text;
    geo_text << geo.rdbuf();
    const std::string t = geo_text.str();
    CHECK(t.find("CreateTopology;") != std::string::npos);
    CHECK(t.find("Physical Surface(6) = {6};") != std::string::npos);
    CHECK(t.find("Mesh.MeshSizeMax = 2.5;") != std::string::npos);
    CHECK(t.find("Field[2] = Threshold;") != std::string::npos);
    CHECK(t.find("Field[2].SizeMin = 0.5;") != std::string::npos);
    CHECK(t.find("Background Field = 3;") != std::string::npos);
}

TEST_CASE("a Gmsh 2.2 mesh reads, maps faces and promotes to quadratic", "[sim][format]")
{
    std::string dir = work_dir("msh_read");
    std::string path = (fs::u8path(dir) / "one.msh").u8string();
    std::ofstream(fs::u8path(path), std::ios::binary) << k_one_tet_msh;

    TetMesh     linear;
    std::string error;
    REQUIRE(read_gmsh_msh22(path, 4, linear, error));
    CHECK(linear.nodes.size() == 4); // the stray node 99 is dropped
    CHECK(linear.tets.size() == 1);
    CHECK(linear.boundary.size() == 4);
    CHECK(linear.boundary_face == std::vector<uint32_t>{ 0, 1, 2, 3 });

    QuadraticMesh q;
    REQUIRE(promote_to_quadratic(linear, q, error));
    CHECK(q.corner_count == 4);
    CHECK(q.nodes.size() == 10);
    // Outward: every boundary triangle's normal points away from the tetrahedron's centroid.
    for (const auto& b : q.boundary) {
        const Vec3 &p = q.nodes[b[0]], &r1 = q.nodes[b[1]], &r2 = q.nodes[b[2]];
        Vec3 u{ r1[0] - p[0], r1[1] - p[1], r1[2] - p[2] }, v{ r2[0] - p[0], r2[1] - p[1], r2[2] - p[2] };
        Vec3 n{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        Vec3 c{ 0.25 - p[0], 0.25 - p[1], 0.25 - p[2] };
        CHECK(n[0] * c[0] + n[1] * c[1] + n[2] * c[2] < 0);
        // Mid-edge nodes sit halfway along their edges.
        for (int e = 0; e < 3; ++e) {
            const Vec3& a = q.nodes[b[e]];
            const Vec3& z = q.nodes[b[(e + 1) % 3]];
            const Vec3& m = q.nodes[b[3 + e]];
            for (int k = 0; k < 3; ++k)
                CHECK(m[k] == Approx((a[k] + z[k]) / 2));
        }
    }
    // Node 9 of a C3D10 is the middle of edge (2, 3).
    const auto& t = q.tets[0];
    for (int k = 0; k < 3; ++k)
        CHECK(q.nodes[t[9]][k] == Approx((q.nodes[t[2]][k] + q.nodes[t[3]][k]) / 2));
}

TEST_CASE("a mesh whose boundary leaves out a face is refused", "[sim][format]")
{
    TetMesh m;
    m.nodes         = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    m.tets          = { { 0, 1, 2, 3 } };
    m.boundary      = { { 0, 1, 2 }, { 0, 1, 3 }, { 1, 2, 3 } };
    m.boundary_face = { 0, 1, 2 };
    QuadraticMesh q;
    std::string   error;
    CHECK_FALSE(promote_to_quadratic(m, q, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("CalculiX input holds the fixture and spreads the force", "[sim][format]")
{
    std::string dir  = work_dir("inp_write");
    std::string path = (fs::u8path(dir) / "one.msh").u8string();
    std::ofstream(fs::u8path(path), std::ios::binary) << k_one_tet_msh;
    TetMesh       linear;
    QuadraticMesh q;
    std::string   error;
    REQUIRE(read_gmsh_msh22(path, 4, linear, error));
    REQUIRE(promote_to_quadratic(linear, q, error));

    StaticStudy s;
    s.material = { "PLA", 3500, 0.36, 1.24e-9 };
    s.fixtures = { { { 0 } } };
    s.loads    = { { { 2 }, { 3, 0, -6 } } };
    REQUIRE(write_calculix_static(dir, q, s, error));

    std::ifstream in(fs::u8path(dir) / "part.inp");
    std::string   line, section;
    double        sum[3] = { 0, 0, 0 };
    std::set<int> fixed;
    bool          elastic = false;
    while (std::getline(in, line)) {
        if (line.rfind("*", 0) == 0) {
            section = line;
            continue;
        }
        if (section == "*CLOAD") {
            int    node, dof;
            double v;
            char   c;
            std::istringstream(line) >> node >> c >> dof >> c >> v;
            sum[dof - 1] += v;
        } else if (section == "*NSET, NSET=FIXED") {
            std::istringstream ls(line);
            int                n;
            char               c;
            while (ls >> n) {
                fixed.insert(n);
                ls >> c;
            }
        } else if (section == "*ELASTIC")
            elastic = line == "3500,0.36";
    }
    CHECK(sum[0] == Approx(3));
    CHECK(sum[1] == Approx(0).margin(1e-12));
    CHECK(sum[2] == Approx(-6));
    CHECK(fixed.size() == 6); // the three corners and three mid-edge nodes of face 0
    CHECK(elastic);
}

TEST_CASE("the .frd reader takes displacement, stress and force", "[sim][format]")
{
    std::string dir = work_dir("frd_read");
    std::ofstream(fs::u8path(dir) / "part.frd", std::ios::binary)
        << "    1C\n"
           "    2C                             2                                     1\n"
           " -1         1 0.00000E+00 0.00000E+00 0.00000E+00\n"
           " -3\n"
           "  100CL  101 1.000000000           2                     0    1           1\n"
           " -4  DISP        4    1\n"
           " -5  D1          1    2    1    0\n"
           " -1         1 1.00000E-01-2.00000E-01 3.00000E-01\n"
           " -1         2-1.50000E+00 0.00000E+00 2.50000E-03\n"
           " -3\n"
           " -4  FORC        4    1\n"
           " -1         1-5.00000E+00 0.00000E+00 1.00000E+01\n"
           " -3\n"
           " -4  STRESS      6    1\n"
           " -1         1 1.00000E+01 0.00000E+00 0.00000E+00 0.00000E+00 0.00000E+00 0.00000E+00\n"
           " -1         2 1.00000E+00 2.00000E+00 3.00000E+00 4.00000E+00 5.00000E+00 6.00000E+00\n"
           " -3\n"
           " 9999\n";
    StaticResult r;
    std::string  error;
    REQUIRE(read_calculix_static(dir, 2, r, error));
    CHECK(r.displacement[0][1] == Approx(-0.2));
    CHECK(r.displacement[1][0] == Approx(-1.5));
    CHECK(r.displacement[1][2] == Approx(0.0025));
    CHECK(r.force[0][2] == Approx(10));
    CHECK(r.force[1][0] == 0);
    CHECK(von_mises(r.stress[0]) == Approx(10));
    CHECK(r.stress[1][5] == Approx(6));

    CHECK_FALSE(read_calculix_static(dir, 3, r, error)); // node 3 has no result
}

TEST_CASE("a result file round-trips", "[sim][format]")
{
    std::string   dir = work_dir("bsr");
    SurfaceResult r;
    r.positions  = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    r.triangles  = { 0, 1, 2 };
    r.face_index = { 7 };
    r.fields.push_back({ "vonMises", 1, "MPa", 1, 3, { 1, 2, 3 } });
    r.fields.push_back({ "displacement", 3, "mm", 0, 1, { 0, 0, 0, 1, 0, 0, 0, 0, 0.5f } });
    r.summary_json = R"({"maxVonMises":3})";
    const std::string path = (fs::u8path(dir) / "result.bsr").u8string();
    std::string       error;
    REQUIRE(write_result_file(path, r, error));
    CHECK(fs::file_size(fs::u8path(path)) % 4 == 0);

    SurfaceResult back;
    REQUIRE(read_result_file(path, back, error));
    CHECK(back.positions == r.positions);
    CHECK(back.triangles == r.triangles);
    CHECK(back.face_index == r.face_index);
    REQUIRE(back.fields.size() == 2);
    CHECK(back.fields[1].name == "displacement");
    CHECK(back.fields[1].values == r.fields[1].values);
    CHECK(nlohmann::json::parse(back.summary_json)["maxVonMises"] == 3);
}

TEST_CASE("job folders are made empty and only valid ids are accepted", "[sim][format]")
{
    std::string dir = make_job_dir(SIM_TEST_WORK, "reuse");
    REQUIRE_FALSE(dir.empty());
    std::ofstream(fs::u8path(dir) / "old.txt") << "x";
    dir = make_job_dir(SIM_TEST_WORK, "reuse");
    CHECK_FALSE(fs::exists(fs::u8path(dir) / "old.txt"));
    CHECK(make_job_dir(SIM_TEST_WORK, "..").empty());
    CHECK(make_job_dir(SIM_TEST_WORK, "a/b").empty());
}
