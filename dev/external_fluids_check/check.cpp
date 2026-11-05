#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "CoolProp/AbstractState.h"
#include "CoolProp/CoolProp.h"

using namespace CoolProp;

static int errors = 0;
template <class F>
static void call(const char* what, F f) {
    try {
        double v = f();
        std::printf("  %s = %.17g\n", what, v);
    } catch (std::exception& e) {
        errors++;
        std::printf("  %s: E %s\n", what, e.what());
    }
}

static void grid(const std::string& fluid) {
    std::printf("first PropsSI %.17g\n", PropsSI("D", "T", 300, "P", 101325, "Water"));
    std::shared_ptr<AbstractState> st(AbstractState::factory("HEOS", fluid));
    const parameters trivial[] = {iT_critical, iP_critical, irhomass_critical, imolar_mass, iT_triple, iT_max, iP_max, iacentric_factor, iT_reducing};
    for (auto p : trivial) call(get_parameter_information(p, "short").c_str(), [&] { return st->trivial_keyed_output(p); });
    const double Tc = st->trivial_keyed_output(iT_critical), pc = st->trivial_keyed_output(iP_critical);
    struct S { const char* name; input_pairs pair; double a, b; };
    const std::vector<S> states = {{"PT", PT_INPUTS, 0.5 * pc, 1.5 * Tc}, {"PT", PT_INPUTS, 101325, 0.9 * Tc}, {"PT", PT_INPUTS, 2 * pc, 0.75 * Tc},
                                   {"QT", QT_INPUTS, 0, 0.8 * Tc},       {"QT", QT_INPUTS, 1, 0.8 * Tc},     {"PQ", PQ_INPUTS, 0.3 * pc, 0.5}};
    const parameters outs[] = {iDmass, iHmass, iSmass, iCpmass, iCvmass, ispeed_sound, iviscosity, iconductivity, iPrandtl, iZ, iUmass, iisobaric_expansion_coefficient};
    for (std::size_t k = 0; k < states.size(); ++k) {
        const S& s = states[k];
        std::printf("s%zu %s %.17g %.17g\n", k, s.name, s.a, s.b);
        try { st->update(s.pair, s.a, s.b); } catch (std::exception& e) { errors++; std::printf("  update: E %s\n", e.what()); continue; }
        for (auto p : outs) call(get_parameter_information(p, "short").c_str(), [&] { return st->keyed_output(p); });
        call("T", [&] { return st->keyed_output(iT); });
        call("P", [&] { return st->keyed_output(iP); });
        if (std::string(s.name) != "PT") call("sigma", [&] { return st->keyed_output(isurface_tension); });
        double h, sm, p, d, T;
        try { h = st->hmass(); sm = st->smass(); p = st->p(); d = st->rhomass(); T = st->T(); } catch (std::exception& e) { std::printf("  state: E %s\n", e.what()); continue; }
        const struct { const char* name; input_pairs pair; double x, y; } flashes[] = {{"HP", HmassP_INPUTS, h, p}, {"PS", PSmass_INPUTS, p, sm}, {"DT", DmassT_INPUTS, d, T}};
        for (const auto& f : flashes) {
            std::printf(" %s flash %.17g %.17g\n", f.name, f.x, f.y);
            try { st->update(f.pair, f.x, f.y); } catch (std::exception& e) { errors++; std::printf("  update: E %s\n", e.what()); continue; }
            call("T", [&] { return st->T(); });
            call("D", [&] { return st->rhomass(); });
        }
    }
    std::printf("done, %d errors\n", errors);
}

#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "Backends/Helmholtz/Fluids/FluidLibrary.h"

static std::string directory;
static std::atomic<int> requests{0};
static void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
static std::string read_data(const std::string& file) {
    ++requests;
    std::ifstream input(directory + "/" + file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + file);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
template <class F> static void expect_failure(F f, const std::string& part) {
    try { f(); }
    catch (const ValueError& e) {
        require(std::string(e.what()).find(part) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error("Expected ValueError containing " + part);
}
static void extras() {
    for (const std::string name : {"water", "CO2", "R134A", "R1234ze(E)", "n-Butane", "Water-SRK", "Water-PengRobinson",
                                    "HEOS::Methane[0.4]&Ethane[0.6]", "HEOS::R32[0.3]&R125[0.7]", "Air.mix"}) {
        std::printf("fluid %s\n", name.c_str());
        const double density = PropsSI("D", "T", 300, "P", 101325, name);
        require(ValidNumber(density), "PropsSI failed for " + name + ": " + get_global_param_string("errstring"));
        std::printf("PropsSI D %.17g\n", density);
        std::printf("PropsSI error %s\n", get_global_param_string("errstring").c_str());
    }
    call("invalid PropsSI", [] { return PropsSI("D", "T", -1, "P", 101325, "Water"); });
    std::printf("invalid PropsSI error %s\n", get_global_param_string("errstring").c_str());
    // Exercise a pending reference-state mutation before any state is constructed for this fluid.
    set_fluid_enthalpy_entropy_offset("Ammonia", 0.01, -0.02, "check");
    call("offset h", [] { return PropsSI("H", "T", 300, "P", 101325, "Ammonia"); });
    call("offset s", [] { return PropsSI("S", "T", 300, "P", 101325, "Ammonia"); });
    auto listing = cpjson::parse(get_fluid_as_JSONstring("Water"));
    const auto size = strsplit(get_fluid_list(), ',').size();
    expect_failure([&] { get_library().add_many(listing); }, "OVERWRITE_FLUIDS");
    require(strsplit(get_fluid_list(), ',').size() == size, "Duplicate changed fluid list");
    set_config_bool(OVERWRITE_FLUIDS, true);
    get_library().add_many(listing);
    require(strsplit(get_fluid_list(), ',').size() == size, "Overwrite changed fluid list");
    call("overwrite Water", [] { return PropsSI("D", "T", 300, "P", 101325, "Water"); });
    listing[0]["INFO"]["NAME"] = "TestFluid";
    listing[0]["INFO"]["CAS"] = "test-fluid";
    listing[0]["INFO"]["ALIASES"] = std::vector<std::string>{"testfluid"};
    get_library().add_many(listing);
    require(strsplit(get_fluid_list(), ',').size() == size + 1, "New fluid lost reserved slots");
    call("new fluid", [] { return PropsSI("D", "T", 300, "P", 101325, "testfluid"); });
    // Overwrite an indexed fluid that has never been requested.
    auto water = listing[0];
    water["INFO"]["NAME"] = "Xenon";
    water["INFO"]["CAS"] = "7440-63-3";
    water["INFO"]["ALIASES"] = std::vector<std::string>{"xenon", "XENON"};
    get_library().add_one(water);
    call("pending overwrite", [] { return PropsSI("D", "T", 300, "P", 101325, "Xenon"); });
    std::puts("extras passed");
}

int main(int argc, char** argv) {
    try {
        require(argc >= 4, "Usage: check embedded|external action data-directory [fluid]");
        const bool external = std::string(argv[1]) == "external";
        const std::string action = argv[2];
        directory = argv[3];
        if (external && action != "failure" && action != "recursive") {
            set_fluid_data_provider(read_data);
            set_fluid_data_provider({});
            set_fluid_data_provider(read_data);
        }
        if (action == "list") {
            std::puts(get_fluid_list().c_str());
            if (external) require(requests == 0, "FluidsList loaded fluid data");
        } else if (action == "grid") {
            require(argc == 5, "grid needs fluid");
            grid(argv[4]);
        } else if (action == "json") {
            for (const auto& name : strsplit(get_fluid_list(), ',')) {
                const auto json = get_fluid_as_JSONstring(name);
                std::printf("%s %s\n", name.c_str(), json.c_str());
            }
        } else if (action == "aliases") {
            std::size_t n = 0;
            for (const auto& name : strsplit(get_fluid_list(), ',')) {
                const auto fluid = get_fluid(name);
                std::vector<std::string> keys = {fluid.name, fluid.CAS};
                for (const auto& alias : fluid.aliases) { keys.push_back(alias); keys.push_back(upper(alias)); }
                for (const auto& key : keys) {
                    const auto found = get_fluid(key);
                    std::printf("%s %s %.17g\n", key.c_str(), found.name.c_str(), static_cast<double>(found.EOS().molar_mass));
                    ++n;
                }
            }
            std::printf("%zu identifier lookups\n", n);
        } else if (action == "extras") {
            extras();
        } else if (action == "lifecycle") {
            get_fluid_list();
            expect_failure([] { set_fluid_data_provider({}); }, "after");
            expect_failure([] { set_fluid_data_provider(read_data); }, "after");
            std::puts("provider lifecycle passed");
        } else if (action == "lazy") {
            require(external, "lazy needs external");
            const auto names = strsplit(get_fluid_list(), ',');
            require(requests == 0, "Index fetched payloads");
            const auto first = get_library().get(std::size_t(0));
            require(first.name == names.at(0) && requests == 1, "Indexed get did not load one fluid");
            get_fluid_as_JSONstring(first.name);
            get_fluid(first.CAS);
            require(requests == 1, "Cached fluid fetched twice");
            expect_failure([] { set_fluid_data_provider({}); }, "after");
            expect_failure([] { set_fluid_data_provider(read_data); }, "after");
            std::puts("lazy and lifecycle passed");
        } else if (action == "failure") {
            int attempt = 0;
            set_fluid_data_provider([&](const std::string& file) {
                switch (++attempt) {
                    case 1: throw std::runtime_error("unavailable");
                    case 2: return std::string();
                    case 3: return std::string("invalid CBOR");
                    case 4: return std::string("\xa0", 1); // valid CBOR, invalid fluid schema
                    case 5:
                    case 7:
                    case 8:
                    case 9:
                    case 10: {
                        const auto data = read_data(file);
                        auto j = cpjson::from_cbor(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
                        if (attempt == 5) j["INFO"]["NAME"] = "Other";
                        if (attempt == 7) j["EOS"] = nlohmann::json::array();
                        if (attempt == 8) j["EOS"] = nlohmann::json::object();
                        if (attempt == 9) j["INFO"]["CAS"] = "";
                        if (attempt == 10) j["INFO"]["NAME"] = "";
                        const auto bytes = nlohmann::json::to_cbor(j);
                        return std::string(bytes.begin(), bytes.end());
                    }
                    case 6: throw 42;
                    default: return read_data(file);
                }
            });
            for (int i = 0; i < 10; ++i) expect_failure([] { get_fluid("Water"); }, "Water");
            require(get_fluid("water").name == "Water" && attempt == 11, "Retry failed");
            get_fluid_as_JSONstring("Water");
            require(attempt == 11, "Successful retry not cached");
            std::puts("provider failures and retry passed");
        } else if (action == "recursive") {
            set_fluid_data_provider([](const std::string& file) {
                get_fluid("Water");
                return read_data(file);
            });
            expect_failure([] { get_fluid("Water"); }, "Recursive");
            std::puts("recursive provider rejected");
        } else if (action == "threads") {
            const auto names = std::vector<std::string>{"Water", "CarbonDioxide", "Nitrogen", "R134a"};
            std::atomic<int> failed{0};
            std::vector<std::thread> threads;
            for (int i = 0; i < 16; ++i) threads.emplace_back([&, i] {
                try {
                    for (int j = 0; j < 10; ++j) {
                        auto f = get_fluid(names[i % names.size()]);
                        require(f.name == names[i % names.size()], "Wrong thread fluid");
                        require(!get_fluid_as_JSONstring(f.name).empty(), "Missing thread JSON");
                    }
                } catch (...) { ++failed; }
            });
            for (auto& t : threads) t.join();
            require(failed == 0, "Concurrent cold loading failed");
            if (external) require(requests == 4, "Concurrent loading fetched duplicates");
            std::puts("concurrent cold loading passed");
        } else if (action == "rss") {
            // Reset inherited peak accounting by forking from this small native
            // process before fluid loading, rather than from the Python runner.
            const pid_t child = fork();
            require(child >= 0, "fork failed");
            if (child > 0) {
                int status = 0;
                require(waitpid(child, &status, 0) == child, "waitpid failed");
                require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "RSS child failed");
                return 0;
            }
            const auto f = get_fluid("Water");
            std::printf("Water density %.17g\n", PropsSI("D", "T", 300, "P", 101325, f.name));
            struct rusage usage;
            require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
            std::printf("max RSS %ld KiB\n", usage.ru_maxrss);
            if (external) require(requests == 1, "Single fluid fetched extra data");
        } else if (action == "missing") {
            expect_failure([] { get_fluid_list(); }, "COOLPROP_EXTERNAL_FLUIDS requires");
            set_fluid_data_provider(read_data);
            require(get_fluid("Water").name == "Water", "Install after missing provider failed");
            std::puts("missing provider and recovery passed");
        } else { throw std::runtime_error("Unknown action"); }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "CHECK FAILED: %s\n", e.what());
        return 1;
    }
}
