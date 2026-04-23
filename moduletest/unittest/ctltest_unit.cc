// ctltest_unit — C++ unit tests for ctltest_core's public API. Separate
// from the self-test YAML suites under moduletest/tests/selftest/, which
// exercise the framework end-to-end against real CTL modules. This binary
// drives the pure-library pieces directly:
//
//   * Value / Tolerance construction and merge semantics
//   * Oracle comparison (abs / rel / ulp, half rounding, tree walk)
//   * Tolerance path validation
//   * CsvTable parsing (quoting, CRLF, comments) + cellToValue heuristics
//   * YamlLoader happy/sad paths
//   * Marshal round-trip across every scalar kind on flat + nested types
//     (exercises the v1.1 CtlType::childElementV fix end-to-end)
//
// Written as a single hand-rolled harness that matches the style of
// unittest/IlmCtl/main.cpp — one `TEST(name)` per suite, early abort on
// CHECK failure so regressions are visible in both Debug and Release.

#include "CaseModel.h"
#include "CsvTable.h"
#include "InterpRunner.h"
#include "Marshal.h"
#include "Oracle.h"
#include "Result.h"
#include "YamlLoader.h"

#include <half.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ctltest;

namespace {

int g_failures = 0;
const char* g_current = "<none>";

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr,                                           \
                         "FAIL [%s] %s:%d  CHECK(%s)\n",                   \
                         g_current, __FILE__, __LINE__, #cond);            \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        auto _a = (a); auto _b = (b);                                      \
        if (!((_a) == (_b))) {                                             \
            std::ostringstream _os;                                        \
            _os << "FAIL [" << g_current << "] " << __FILE__ << ":"        \
                << __LINE__ << "  CHECK_EQ(" #a ", " #b ")  got=" << _a    \
                << " want=" << _b << "\n";                                 \
            std::fprintf(stderr, "%s", _os.str().c_str());                 \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

struct Section {
    explicit Section(const char* name) { g_current = name; std::cout << "  " << name << std::endl; }
    ~Section() { g_current = "<none>"; }
};

// ---------------------------------------------------------------- Value

void testValue()
{
    Section s("Value");

    Value b = Value::makeBool(true);
    CHECK(b.kind == Value::Kind::Bool);
    CHECK(b.b == true);

    Value i = Value::makeInt(-7);
    CHECK(i.kind == Value::Kind::Int);
    CHECK_EQ(i.i, -7);

    Value u = Value::makeUInt(42u);
    CHECK(u.kind == Value::Kind::UInt);
    CHECK_EQ(u.u, 42u);

    Value f = Value::makeFloat(0.25);
    CHECK(f.kind == Value::Kind::Float);
    CHECK(f.f == 0.25);

    Value h = Value::makeHalf(0.5);
    CHECK(h.kind == Value::Kind::Half);
    CHECK(h.f == 0.5);

    Value str = Value::makeString("hello");
    CHECK(str.kind == Value::Kind::String);
    CHECK(str.s == "hello");

    Value seq = Value::makeSeq({ Value::makeFloat(1.0), Value::makeFloat(2.0) });
    CHECK(seq.kind == Value::Kind::Seq);
    CHECK_EQ(seq.seq.size(), size_t(2));

    Value mp = Value::makeMap({ {"x", Value::makeFloat(1.0)},
                                {"y", Value::makeFloat(2.0)} });
    CHECK(mp.kind == Value::Kind::Map);
    CHECK_EQ(mp.map.size(), size_t(2));

    // describe() must be non-empty for leaves.
    CHECK(!f.describe().empty());
    CHECK(!str.describe().empty());
    CHECK(!seq.describe().empty());
}

// ---------------------------------------------------------------- Tolerance::merge

void testToleranceMerge()
{
    Section s("Tolerance::merge");

    Tolerance base;
    base.abs = 1.0e-5;
    base.rel = 1.0e-4;

    Tolerance override1;
    override1.abs = 5.0e-4;

    // Override replaces fields it sets; leaves the rest from base.
    Tolerance merged = Tolerance::merge(base, override1);
    CHECK(merged.abs.has_value());
    CHECK(*merged.abs == 5.0e-4);
    CHECK(merged.rel.has_value());
    CHECK(*merged.rel == 1.0e-4);

    // Override with no fields set returns base semantics intact.
    Tolerance merged_empty = Tolerance::merge(base, Tolerance{});
    CHECK(merged_empty.abs.has_value() && *merged_empty.abs == 1.0e-5);
    CHECK(merged_empty.rel.has_value() && *merged_empty.rel == 1.0e-4);

    // per_field maps from base persist through merge with an override
    // that carries no per_field entries.
    Tolerance with_pf;
    with_pf.abs = 1.0e-5;
    with_pf.per_field["out/r"].abs = 3.0e-4;
    Tolerance merged_pf = Tolerance::merge(with_pf, override1);
    CHECK(merged_pf.per_field.count("out/r"));
}

// ---------------------------------------------------------------- compareTyped

void testCompareTyped()
{
    Section s("compareTyped");

    auto compareOne = [](const Value& exp, const Value& got, const Tolerance& t) {
        std::vector<Diagnostic> out;
        compareTyped("return", exp, got, t, "test.tolerance", out);
        return out;
    };

    // Exact match passes.
    {
        Tolerance t; t.abs = 1.0e-6;
        auto d = compareOne(Value::makeFloat(1.0), Value::makeFloat(1.0), t);
        CHECK(d.empty());
    }

    // abs tolerance passing just within bound.
    {
        Tolerance t; t.abs = 1.0e-3;
        auto d = compareOne(Value::makeFloat(1.0), Value::makeFloat(1.0005), t);
        CHECK(d.empty());
    }

    // abs tolerance failing beyond bound.
    {
        Tolerance t; t.abs = 1.0e-5;
        auto d = compareOne(Value::makeFloat(1.0), Value::makeFloat(1.1), t);
        CHECK(!d.empty());
        CHECK(d.front().path == "return");
    }

    // rel tolerance passes where abs alone would fail.
    {
        Tolerance t; t.rel = 0.01;  // 1%
        auto d = compareOne(Value::makeFloat(1000.0), Value::makeFloat(1005.0), t);
        CHECK(d.empty());
    }

    // Passing condition is "abs OR rel OR ulp" (any one satisfies).
    {
        Tolerance t; t.abs = 1.0e-9; t.rel = 1.0;  // 100% rel -> always passes
        auto d = compareOne(Value::makeFloat(1.0), Value::makeFloat(2.0), t);
        CHECK(d.empty());
    }

    // Half rounding: an authored expected of 0.1 must be rounded to the
    // nearest-even half (≈0.099975586) before comparison. The Half branch of
    // compareTyped fires when `got.kind == Half` (mirroring the real path
    // where a CTL function returns a half and fromArg rebuilds it as Kind::Half).
    {
        Tolerance t; t.abs = 1.0e-6;
        // Author expected as Float 0.1 (raw, as one would write in YAML).
        // Observed value is Half carrying the nearest-even half of 0.1 —
        // after rounding, both sides reduce to the same representable half.
        Value exp = Value::makeFloat(0.1);
        Value got = Value::makeHalf(static_cast<double>(static_cast<float>(
                                         static_cast<half>(0.1f))));
        auto d = compareOne(exp, got, t);
        CHECK(d.empty());
    }

    // Type mismatch (string vs. number) is a diagnostic, not a hard throw.
    {
        Tolerance t; t.abs = 1.0e-6;
        auto d = compareOne(Value::makeString("1.0"), Value::makeFloat(1.0), t);
        CHECK(!d.empty());
    }

    // Nested sequence: path uses "[i]" notation; one failing leaf produces
    // one diagnostic.
    {
        Tolerance t; t.abs = 1.0e-6;
        Value exp = Value::makeSeq({ Value::makeFloat(1.0), Value::makeFloat(2.0) });
        Value got = Value::makeSeq({ Value::makeFloat(1.0), Value::makeFloat(9.0) });
        auto d = compareOne(exp, got, t);
        CHECK_EQ(d.size(), size_t(1));
        CHECK(d.front().path.find("[1]") != std::string::npos);
    }

    // Nested map: path uses "/field" notation per v1.1.
    {
        Tolerance t; t.abs = 1.0e-6;
        Value exp = Value::makeMap({ {"x", Value::makeFloat(1.0)},
                                     {"y", Value::makeFloat(2.0)} });
        Value got = Value::makeMap({ {"x", Value::makeFloat(1.0)},
                                     {"y", Value::makeFloat(9.0)} });
        auto d = compareOne(exp, got, t);
        CHECK_EQ(d.size(), size_t(1));
        CHECK(d.front().path.find("y") != std::string::npos);
    }
}

// ---------------------------------------------------------------- validateTolerancePaths

void testValidateTolerancePaths()
{
    Section s("validateTolerancePaths");

    // Valid per_field on an FP leaf: no diagnostic. Path syntax is the
    // dotted/bracketed form understood by resolvePath (return.x, out[2]).
    {
        Tolerance t;
        t.abs = 1.0e-6;
        t.per_field["return.x"].abs = 1.0e-4;
        Value shape = Value::makeMap(
            { {"return", Value::makeMap({{"x", Value::makeFloat(0.0)},
                                         {"y", Value::makeFloat(0.0)}})} });
        std::vector<Diagnostic> out;
        validateTolerancePaths("", t, shape, out);
        CHECK(out.empty());
    }

    // per_field pointing to a missing path: diagnostic.
    {
        Tolerance t;
        t.per_field["return.missing"].abs = 1.0e-4;
        Value shape = Value::makeMap(
            { {"return", Value::makeMap({{"x", Value::makeFloat(0.0)}})} });
        std::vector<Diagnostic> out;
        validateTolerancePaths("", t, shape, out);
        CHECK(!out.empty());
    }

    // per_field pointing at a non-FP leaf (bool/int/string): diagnostic.
    {
        Tolerance t;
        t.per_field["return.flag"].abs = 1.0e-4;
        Value shape = Value::makeMap(
            { {"return", Value::makeMap({{"flag", Value::makeBool(true)}})} });
        std::vector<Diagnostic> out;
        validateTolerancePaths("", t, shape, out);
        CHECK(!out.empty());
    }
}

// ---------------------------------------------------------------- CsvTable

std::string writeTempFile(const std::string& body, const std::string& suffix)
{
    std::string path = std::string("/tmp/ctltest_unit_") + suffix;
    std::ofstream f(path);
    f << body;
    f.close();
    return path;
}

void testCsvTable()
{
    Section s("CsvTable");

    // Header + two data rows, LF line endings.
    {
        std::string p = writeTempFile("a,b,c\n1,2,3\n4,5,6\n", "basic.csv");
        CsvTable t = readCsv(p);
        CHECK_EQ(t.columns.size(), size_t(3));
        CHECK(t.columns[0] == "a");
        CHECK(t.columns[2] == "c");
        CHECK_EQ(t.rows.size(), size_t(2));
        CHECK(t.rows[1][2] == "6");
        CHECK_EQ(t.colIndex("b"), size_t(1));
        CHECK_EQ(t.colIndex("missing"), size_t(-1));
    }

    // Quoted fields with embedded comma + escaped quote.
    {
        std::string body = "name,note\n"
                           "\"smith, j\",\"he said \"\"hi\"\"\"\n";
        std::string p = writeTempFile(body, "quoted.csv");
        CsvTable t = readCsv(p);
        CHECK(t.rows[0][0] == "smith, j");
        CHECK(t.rows[0][1] == "he said \"hi\"");
    }

    // Comment lines are dropped.
    {
        std::string body = "# top comment\nx,y\n# middle comment\n1,2\n";
        std::string p = writeTempFile(body, "comments.csv");
        CsvTable t = readCsv(p);
        CHECK_EQ(t.columns.size(), size_t(2));
        CHECK_EQ(t.rows.size(), size_t(1));
    }

    // CRLF line endings.
    {
        std::string p = writeTempFile("a,b\r\n1,2\r\n", "crlf.csv");
        CsvTable t = readCsv(p);
        CHECK_EQ(t.columns.size(), size_t(2));
        CHECK_EQ(t.rows.size(), size_t(1));
        CHECK(t.rows[0][1] == "2");
    }

    // cellToValue heuristics.
    Value vb = cellToValue("true");
    CHECK(vb.kind == Value::Kind::Bool);

    Value vi = cellToValue("-17");
    CHECK(vi.kind == Value::Kind::Int);

    Value vf = cellToValue("3.14");
    CHECK(vf.kind == Value::Kind::Float);

    Value vs = cellToValue("hello");
    CHECK(vs.kind == Value::Kind::String);

    Value ve = cellToValue("");
    CHECK(ve.kind == Value::Kind::String);

    // tableAsRows round-trip.
    {
        std::string p = writeTempFile("x,y\n1.0,2.0\n3.0,4.0\n", "rows.csv");
        CsvTable t = readCsv(p);
        auto rows = tableAsRows(t);
        CHECK_EQ(rows.size(), size_t(2));
        CHECK(rows[0].at("x").kind == Value::Kind::Float);
        CHECK(rows[1].at("y").f == 4.0);
    }
}

// ---------------------------------------------------------------- YamlLoader

void testYamlLoader()
{
    Section s("YamlLoader");

    // Minimal happy path: one case with inline oracle.
    {
        std::string body =
            "version: 1\n"
            "suite: unit-test-suite\n"
            "modules: [st_scalar]\n"
            "tests:\n"
            "  - id: a\n"
            "    function: st_scalar::identity_f\n"
            "    inputs: { x: 0.25 }\n"
            "    oracle: { inline: { return: 0.25 } }\n";
        std::string p = writeTempFile(body, "min.yaml");
        Suite suite = loadSuite(p);
        CHECK_EQ(suite.tests.size(), size_t(1));
        CHECK(suite.tests[0].id == "a");
        CHECK(suite.tests[0].mode == TestCase::Mode::Unit);
        CHECK(!suite.tests[0].knownFailure);
        CHECK(suite.tests[0].returnsName == "return");
    }

    // known_failure + returns_name + tolerance inheritance.
    {
        std::string body =
            "version: 1\n"
            "modules: [st_scalar]\n"
            "defaults: { tolerance: { abs: 1.0e-5 } }\n"
            "tests:\n"
            "  - id: kf\n"
            "    function: st_scalar::identity_f\n"
            "    inputs: { x: 0.25 }\n"
            "    returns_name: out\n"
            "    known_failure: true\n"
            "    oracle: { inline: { out: 0.25 } }\n";
        std::string p = writeTempFile(body, "kf.yaml");
        Suite suite = loadSuite(p);
        CHECK(suite.tests[0].knownFailure);
        CHECK(suite.tests[0].returnsName == "out");
        CHECK(suite.defaultTolerance.abs.has_value());
    }

    // Load error: malformed YAML produces LoadError with line/col info.
    {
        std::string body = "version: 1\nmodules: [x\n";   // broken sequence
        std::string p = writeTempFile(body, "bad.yaml");
        bool threw = false;
        try { loadSuite(p); }
        catch (const LoadError& e) {
            threw = true;
            CHECK(!e.path().empty());
        }
        CHECK(threw);
    }
}

// ---------------------------------------------------------------- Marshal round-trip

// These tests drive InterpRunner against the existing st_scalar.ctl fixture,
// which is copied into the binary dir by the CMake rule. They validate that
// Marshal + TypeStorage round-trip every scalar kind on flat AND nested
// shapes after the v1.1 parser fix lifted the flat-type restriction.

// InterpRunner has a user-declared dtor which suppresses implicit move, and
// holds a unique_ptr so copy is deleted. Configure via a ref-parameter helper
// rather than returning by value. The fixture is copied next to this binary
// at build time (see moduletest/unittest/CMakeLists.txt).
void initInterp(InterpRunner& r)
{
    r.setModulePaths({"."});
    r.loadModules({"st_scalar"});
}

void testMarshalScalarRoundTrip()
{
    Section s("Marshal scalar round-trip");
    InterpRunner r;
    initInterp(r);

    // float identity
    {
        auto out = r.run("st_scalar::identity_f",
                         {{"x", Value::makeFloat(0.125)}},
                         "return");
        CHECK(out.count("return"));
        CHECK(out["return"].kind == Value::Kind::Float);
        CHECK(out["return"].f == 0.125);
    }

    // int add
    {
        auto out = r.run("st_scalar::add_i",
                         {{"a", Value::makeInt(3)}, {"b", Value::makeInt(4)}},
                         "return");
        CHECK(out["return"].kind == Value::Kind::Int);
        CHECK_EQ(out["return"].i, int64_t(7));
    }

    // bool and
    {
        auto out = r.run("st_scalar::both",
                         {{"a", Value::makeBool(true)}, {"b", Value::makeBool(false)}},
                         "return");
        CHECK(out["return"].kind == Value::Kind::Bool);
        CHECK(out["return"].b == false);
    }

    // half (0.5 is exact)
    {
        auto out = r.run("st_scalar::to_half",
                         {{"x", Value::makeFloat(0.5)}},
                         "return");
        CHECK(out["return"].kind == Value::Kind::Half);
        CHECK(out["return"].f == 0.5);
    }
}

void testMarshalFlatArrayRoundTrip()
{
    Section s("Marshal flat array round-trip");
    InterpRunner r;
    initInterp(r);

    auto out = r.run("st_scalar::lut_sum8",
                     {{"lut", Value::makeSeq({
                         Value::makeFloat(0.0), Value::makeFloat(0.1),
                         Value::makeFloat(0.2), Value::makeFloat(0.3),
                         Value::makeFloat(0.4), Value::makeFloat(0.5),
                         Value::makeFloat(0.6), Value::makeFloat(0.7),
                     })}},
                     "return");
    CHECK(out["return"].kind == Value::Kind::Float);
    CHECK(std::fabs(out["return"].f - 2.8) < 1.0e-5);
}

void testMarshalStructRoundTrip()
{
    Section s("Marshal struct round-trip");
    InterpRunner r;
    initInterp(r);

    auto out = r.run("st_scalar::make_point",
                     {{"x", Value::makeFloat(3.0)}, {"y", Value::makeFloat(4.0)}},
                     "return");
    CHECK(out["return"].kind == Value::Kind::Map);
    CHECK(out["return"].map.count("x"));
    CHECK(out["return"].map.count("y"));
    CHECK(out["return"].map["x"].f == 3.0);
    CHECK(out["return"].map["y"].f == 4.0);
}

void testMarshalNestedArrayRoundTrip()
{
    // v1.1 regression gate: float[3][3] in, float[3][3] out.
    Section s("Marshal nested array round-trip (v1.1)");
    InterpRunner r;
    initInterp(r);

    Value m = Value::makeSeq({
        Value::makeSeq({Value::makeFloat(1.0), Value::makeFloat(2.0), Value::makeFloat(3.0)}),
        Value::makeSeq({Value::makeFloat(4.0), Value::makeFloat(5.0), Value::makeFloat(6.0)}),
        Value::makeSeq({Value::makeFloat(7.0), Value::makeFloat(8.0), Value::makeFloat(9.0)}),
    });

    auto out = r.run("st_scalar::transpose3x3", {{"m", m}}, "return");
    CHECK(out.count("out"));
    const Value& tr = out["out"];
    CHECK(tr.kind == Value::Kind::Seq);
    CHECK_EQ(tr.seq.size(), size_t(3));

    // Expected: transpose of m.
    double expected[3][3] = { {1,4,7}, {2,5,8}, {3,6,9} };
    for (int i = 0; i < 3; ++i) {
        const Value& row = tr.seq[i];
        CHECK(row.kind == Value::Kind::Seq);
        for (int j = 0; j < 3; ++j) {
            CHECK(row.seq[j].f == expected[i][j]);
        }
    }
}

void testMarshalStructWithArrayMemberRoundTrip()
{
    // v1.1: struct containing an array member (struct -> array -> scalar path).
    Section s("Marshal struct-with-array round-trip (v1.1)");
    InterpRunner r;
    initInterp(r);

    auto out = r.run("st_scalar::make_tri",
                     {{"a", Value::makeFloat(1.0)},
                      {"b", Value::makeFloat(2.0)},
                      {"c", Value::makeFloat(3.0)},
                      {"n", Value::makeInt(7)}},
                     "return");
    CHECK(out["return"].kind == Value::Kind::Map);
    const Value& t = out["return"];
    CHECK(t.map.count("v"));
    CHECK(t.map.at("v").kind == Value::Kind::Seq);
    CHECK_EQ(t.map.at("v").seq.size(), size_t(3));
    CHECK(t.map.at("v").seq[0].f == 1.0);
    CHECK(t.map.at("v").seq[2].f == 3.0);
    CHECK(t.map.at("n").kind == Value::Kind::Int);
    CHECK_EQ(t.map.at("n").i, int64_t(7));
}

void testMarshalArrayOfStructRoundTrip()
{
    // v1.1: array of struct (array -> struct -> scalar path).
    Section s("Marshal array-of-struct round-trip (v1.1)");
    InterpRunner r;
    initInterp(r);

    Value q = Value::makeSeq({
        Value::makeMap({{"x", Value::makeFloat(1.0)}, {"y", Value::makeFloat(0.0)}}),
        Value::makeMap({{"x", Value::makeFloat(2.0)}, {"y", Value::makeFloat(0.0)}}),
        Value::makeMap({{"x", Value::makeFloat(3.0)}, {"y", Value::makeFloat(0.0)}}),
        Value::makeMap({{"x", Value::makeFloat(4.0)}, {"y", Value::makeFloat(0.0)}}),
    });
    auto out = r.run("st_scalar::quad_sum_x", {{"q", q}}, "return");
    CHECK(out["return"].kind == Value::Kind::Float);
    CHECK(std::fabs(out["return"].f - 10.0) < 1.0e-5);
}

// ---------------------------------------------------------------- harness

#define TEST(name) \
    do { std::cout << "[ RUN  ] " #name << std::endl; name(); } while (0)

} // namespace

int main(int argc, char* argv[])
{
    auto want = [&](const char* n) {
        return argc < 2 || std::strcmp(argv[1], n) == 0;
    };

    if (want("Value"))                              TEST(testValue);
    if (want("ToleranceMerge"))                     TEST(testToleranceMerge);
    if (want("CompareTyped"))                       TEST(testCompareTyped);
    if (want("ValidateTolerancePaths"))             TEST(testValidateTolerancePaths);
    if (want("CsvTable"))                           TEST(testCsvTable);
    if (want("YamlLoader"))                         TEST(testYamlLoader);
    if (want("MarshalScalar"))                      TEST(testMarshalScalarRoundTrip);
    if (want("MarshalFlatArray"))                   TEST(testMarshalFlatArrayRoundTrip);
    if (want("MarshalStruct"))                      TEST(testMarshalStructRoundTrip);
    if (want("MarshalNestedArray"))                 TEST(testMarshalNestedArrayRoundTrip);
    if (want("MarshalStructWithArray"))             TEST(testMarshalStructWithArrayMemberRoundTrip);
    if (want("MarshalArrayOfStruct"))               TEST(testMarshalArrayOfStructRoundTrip);

    if (g_failures == 0) {
        std::cout << "all unit tests passed" << std::endl;
        return 0;
    }
    std::fprintf(stderr, "%d CHECK failure(s)\n", g_failures);
    return 1;
}
