
#include "FluidLibrary.h"

#include "CoolProp/detail/json.h"
#include <mutex>

#include "Backends/Helmholtz/HelmholtzEOSBackend.h"

#include "fluid_index_CBOR.h"

#ifndef COOLPROP_EXTERNAL_FLUIDS
#if defined(COOLPROP_NO_INCBIN)
#    define INCBIN_CONST
#    define INCBIN_ALIGN
#    include "all_fluids_CBOR.h"
#    undef INCBIN_CONST
#    undef INCBIN_ALIGN
#else
#    include "incbin.h"
// Use the magic of the incbin library to include the binary CBOR data
#    if defined(_MSC_VER)
#        include "all_fluids_CBOR.h"
#    else

INCBIN(all_fluids_CBOR, "all_fluids.cbor");
#    endif
#endif
#endif  // COOLPROP_EXTERNAL_FLUIDS

namespace CoolProp {

static JSONFluidLibrary library;
static std::mutex provider_mutex;
static std::function<std::string(const std::string&)> fluid_data_provider;
static bool library_loading_started = false;

void set_fluid_data_provider(std::function<std::string(const std::string&)> provider) {
    std::lock_guard<std::mutex> lock(provider_mutex);
    if (library_loading_started) {
        throw ValueError("Cannot change the fluid data provider after fluid library loading has started");
    }
    fluid_data_provider = std::move(provider);
}

void load();

// Thread-safe lazy initialization of the global fluid library. The
// previous `if (library.is_empty()) load();` pattern races when multiple
// threads hit a cold cache simultaneously — multiple threads see the
// library empty, all call load() concurrently, and the resulting
// concurrent inserts into string_to_index_map / fluid_map leave the
// library partially populated (gh-2787). std::call_once ensures load()
// runs exactly once even under concurrent first calls; if load() throws,
// the flag is left unset and the next call will retry.
static std::once_flag library_load_flag;

static void ensure_library_loaded() {
    std::call_once(library_load_flag, &load);
}

void load() {
    std::lock_guard<std::mutex> lock(provider_mutex);
#ifdef COOLPROP_EXTERNAL_FLUIDS
    if (!fluid_data_provider) {
        throw ValueError("COOLPROP_EXTERNAL_FLUIDS requires set_fluid_data_provider before loading the fluid library");
    }
#endif
    library_loading_started = true;
    if (getenv("COOLPROP_DISABLE_SUPERANCILLARIES_ENTIRELY")) {
        std::cout << "CoolProp: superancillaries have been disabled because the COOLPROP_DISABLE_SUPERANCILLARIES_ENTIRELY environment variable has "
                     "been defined"
                  << '\n';
    }
    if (fluid_data_provider) {
        library.load_index(cpjson::from_cbor(fluid_index_CBOR, sizeof(fluid_index_CBOR)), fluid_data_provider);
        return;
    }
#ifndef COOLPROP_EXTERNAL_FLUIDS
    // Let a CBOR-decode failure propagate (it did, as a parse error, before this
    // migration): under std::call_once a thrown load() leaves the once-flag unset
    // so the error surfaces and a later call can retry, rather than silently
    // leaving the global library empty. add_many keeps its original catch.
    nlohmann::json dd = cpjson::from_cbor(gall_fluids_CBORData, gall_fluids_CBORSize);
    try {
        library.add_many(dd);
    } catch (std::exception& e) {
        std::cout << e.what() << '\n';
    }
#endif
}

void JSONFluidLibrary::set_fluid_enthalpy_entropy_offset(const std::string& fluid, double delta_a1, double delta_a2, const std::string& ref) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // Try to find it
    auto it = string_to_index_map.find(fluid);
    if (it != string_to_index_map.end()) {
        ensure_fluid_loaded(it->second);
        auto it2 = fluid_map.find(it->second);
        // If it is found
        if (it2 != fluid_map.end()) {
            if (!ValidNumber(delta_a1) || !ValidNumber(delta_a2)) {
                throw ValueError(format("Not possible to set reference state for fluid %s because offset values are NAN", fluid.c_str()));
            }
            it2->second.EOS().alpha0.EnthalpyEntropyOffset.set(delta_a1, delta_a2, ref);

            // Note: cached caloric superancillaries are not invalidated here.
            // They are reference-state-agnostic: the offset's effect on h(T)
            // and s(T) along the saturation curve is exactly a constant
            // (Δh = R·T_red·Δa2, Δs = −R·Δa1), so shapes are invariant and
            // c0 is shifted at query time. The (a1, a2) stamp recorded at
            // first build lets resolve_T_via_superancillary translate the
            // user's target into the cache's frame. See #2773.

            shared_ptr<CoolProp::HelmholtzEOSBackend> HEOS = std::make_shared<CoolProp::HelmholtzEOSBackend>(it2->second);
            HEOS->specify_phase(iphase_gas);  // Something homogeneous;
            // Calculate the new enthalpy and entropy values
            HEOS->update(DmolarT_INPUTS, it2->second.EOS().hs_anchor.rhomolar, it2->second.EOS().hs_anchor.T);
            it2->second.EOS().hs_anchor.hmolar = HEOS->hmolar();
            it2->second.EOS().hs_anchor.smolar = HEOS->smolar();

            double f = (HEOS->name() == "Water" || HEOS->name() == "CarbonDioxide") ? 1.00001 : 1.0;

            // Calculate the new enthalpy and entropy values at the reducing state
            HEOS->update(DmolarT_INPUTS, it2->second.EOS().reduce.rhomolar * f, it2->second.EOS().reduce.T * f);
            it2->second.EOS().reduce.hmolar = HEOS->hmolar();
            it2->second.EOS().reduce.smolar = HEOS->smolar();

            // Calculate the new enthalpy and entropy values at the critical state
            HEOS->update(DmolarT_INPUTS, it2->second.crit.rhomolar * f, it2->second.crit.T * f);
            it2->second.crit.hmolar = HEOS->hmolar();
            it2->second.crit.smolar = HEOS->smolar();

            // Calculate the new enthalpy and entropy values
            HEOS->update(DmolarT_INPUTS, it2->second.triple_liquid.rhomolar, it2->second.triple_liquid.T);
            it2->second.triple_liquid.hmolar = HEOS->hmolar();
            it2->second.triple_liquid.smolar = HEOS->smolar();

            // Calculate the new enthalpy and entropy values
            HEOS->update(DmolarT_INPUTS, it2->second.triple_vapor.rhomolar, it2->second.triple_vapor.T);
            it2->second.triple_vapor.hmolar = HEOS->hmolar();
            it2->second.triple_vapor.smolar = HEOS->smolar();

            if (!HEOS->is_pure()) {
                // Calculate the new enthalpy and entropy values
                HEOS->update(DmolarT_INPUTS, it2->second.EOS().max_sat_T.rhomolar, it2->second.EOS().max_sat_T.T);
                it2->second.EOS().max_sat_T.hmolar = HEOS->hmolar();
                it2->second.EOS().max_sat_T.smolar = HEOS->smolar();
                // Calculate the new enthalpy and entropy values
                HEOS->update(DmolarT_INPUTS, it2->second.EOS().max_sat_p.rhomolar, it2->second.EOS().max_sat_p.T);
                it2->second.EOS().max_sat_p.hmolar = HEOS->hmolar();
                it2->second.EOS().max_sat_p.smolar = HEOS->smolar();
            }
        } else {
            throw ValueError(format("fluid [%s] was not found in JSONFluidLibrary", fluid.c_str()));
        }
    }
}

/// Add all the fluid entries in the JSON-encoded string passed in
void JSONFluidLibrary::add_many(const std::string& JSON_string) {

    // First load all the baseline fluids
    ensure_library_loaded();

    // Then, load the fluids we would like to add
    nlohmann::json doc = cpjson::parse(JSON_string);
    library.add_many(doc);
};

void JSONFluidLibrary::add_many(const nlohmann::json& listing) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!listing.is_array()) {
        add_one(listing);
        return;
    }
    for (const auto& fluid_json : listing) {
        add_one(fluid_json);
    }
};

CoolPropFluid JSONFluidLibrary::parse_fluid(const nlohmann::json& fluid_json) {
    CoolPropFluid fluid;
    try {
        fluid.name = fluid_json.at("INFO").at("NAME").get<std::string>();
        if (fluid.name.empty()) throw ValueError("Fluid name must not be empty");
        // CAS number
        if (!fluid_json.at("INFO").contains("CAS")) {
            throw ValueError(format("fluid [%s] does not have \"CAS\" member", fluid.name.c_str()));
        }
        fluid.CAS = fluid_json.at("INFO").at("CAS").get<std::string>();
        if (fluid.CAS.empty()) throw ValueError("Fluid CAS must not be empty");

        // REFPROP alias
        if (!fluid_json.at("INFO").contains("REFPROP_NAME")) {
            throw ValueError(format("fluid [%s] does not have \"REFPROP_NAME\" member", fluid.name.c_str()));
        }
        fluid.REFPROPname = fluid_json.at("INFO").at("REFPROP_NAME").get<std::string>();

        // FORMULA
        if (fluid_json.at("INFO").contains("FORMULA")) {
            fluid.formula = cpjson::get_string(fluid_json.at("INFO"), "FORMULA");
        } else {
            fluid.formula = "N/A";
        }

        // Abstract references
        if (fluid_json.at("INFO").contains("INCHI_STRING")) {
            fluid.InChI = cpjson::get_string(fluid_json.at("INFO"), "INCHI_STRING");
        } else {
            fluid.InChI = "N/A";
        }

        if (fluid_json.at("INFO").contains("INCHI_KEY")) {
            fluid.InChIKey = cpjson::get_string(fluid_json.at("INFO"), "INCHI_KEY");
        } else {
            fluid.InChIKey = "N/A";
        }

        if (fluid_json.at("INFO").contains("SMILES")) {
            fluid.smiles = cpjson::get_string(fluid_json.at("INFO"), "SMILES");
        } else {
            fluid.smiles = "N/A";
        }

        if (fluid_json.at("INFO").contains("CHEMSPIDER_ID")) {
            fluid.ChemSpider_id = cpjson::get_integer(fluid_json.at("INFO"), "CHEMSPIDER_ID");
        } else {
            fluid.ChemSpider_id = -1;
        }

        if (fluid_json.at("INFO").contains("2DPNG_URL")) {
            fluid.TwoDPNG_URL = cpjson::get_string(fluid_json.at("INFO"), "2DPNG_URL");
        } else {
            fluid.TwoDPNG_URL = "N/A";
        }

        // Parse the environmental parameters
        if (!(fluid_json.at("INFO").contains("ENVIRONMENTAL"))) {
            if (get_debug_level() > 0) {
                std::cout << format("Environmental data are missing for fluid [%s]\n", fluid.name.c_str());
            }
        } else {
            parse_environmental(fluid_json.at("INFO").at("ENVIRONMENTAL"), fluid);
        }

        // Aliases
        fluid.aliases = cpjson::get_string_array(fluid_json.at("INFO").at("ALIASES"));

        // Critical state
        if (!fluid_json.contains("STATES")) {
            throw ValueError(format("fluid [%s] does not have \"STATES\" member", fluid.name.c_str()));
        }
        parse_states(fluid_json.at("STATES"), fluid);

        if (get_debug_level() > 5) {
            std::cout << format("Loading fluid %s with CAS %s\n", fluid.name.c_str(), fluid.CAS.c_str());
        }

        // Reject an empty EOS before downstream parsers access EOSVector[0].
        const auto& eos = fluid_json.at("EOS");
        if (!eos.is_array() || eos.empty()) {
            throw ValueError("EOS must be a non-empty array");
        }
        parse_EOS_listing(eos, fluid);

        // Validate the fluid
        validate(fluid);

        // Ancillaries for saturation
        if (!fluid_json.contains("ANCILLARIES")) {
            throw ValueError(format("Ancillary curves are missing for fluid [%s]", fluid.name.c_str()));
        };
        parse_ancillaries(fluid_json.at("ANCILLARIES"), fluid);

        // Surface tension
        if (!(fluid_json.at("ANCILLARIES").contains("surface_tension"))) {
            if (get_debug_level() > 0) {
                std::cout << format("Surface tension curves are missing for fluid [%s]\n", fluid.name.c_str());
            }
        } else {
            parse_surface_tension(fluid_json.at("ANCILLARIES").at("surface_tension"), fluid);
        }

        // Melting line
        if (!(fluid_json.at("ANCILLARIES").contains("melting_line"))) {
            if (get_debug_level() > 0) {
                std::cout << format("Melting line curves are missing for fluid [%s]\n", fluid.name.c_str());
            }
        } else {
            parse_melting_line(fluid_json.at("ANCILLARIES").at("melting_line"), fluid);
        }

        // Parse the transport property (viscosity and/or thermal conductivity) parameters
        if (!(fluid_json.contains("TRANSPORT"))) {
            default_transport(fluid);
        } else {
            parse_transport(fluid_json.at("TRANSPORT"), fluid);
        }

        return fluid;
    } catch (const std::exception& e) {
        throw ValueError(format("Unable to load fluid [%s] due to error: %s", fluid.name.c_str(), e.what()));
    }
}

void JSONFluidLibrary::register_identifiers(const CoolPropFluid& fluid, std::size_t index) {
    // Keep the embedded and indexed lookup keys identical (including alias case variants).
    string_to_index_map[fluid.CAS] = index;
    string_to_index_map[fluid.name] = index;
    for (const auto& alias : fluid.aliases) {
        string_to_index_map[alias] = index;
        string_to_index_map[upper(alias)] = index;
    }
}

void JSONFluidLibrary::insert_fluid(const nlohmann::json& fluid_json, CoolPropFluid fluid, std::size_t index) {
    // Serialize before publishing the parsed object, so parse/serialization errors are retryable.
    std::string json = fluid_json.dump();
    fluid_map[index] = std::move(fluid);
    JSONstring_map[index] = std::move(json);
    pending.erase(index);
    _is_empty = false;
}

void JSONFluidLibrary::add_one(const nlohmann::json& fluid_json) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    CoolPropFluid fluid = parse_fluid(fluid_json);
    std::size_t index = name_vector.size();
    auto find_index = [&](const std::string& key) {
        auto it = string_to_index_map.find(key);
        if (it == string_to_index_map.end()) return false;
        index = it->second;
        return true;
    };
    if (!find_index(fluid.CAS) && !find_index(fluid.name) && !find_index(upper(fluid.name))) {
        for (const auto& alias : fluid.aliases) {
            if (find_index(alias) || find_index(upper(alias))) break;
        }
    }
    if (index != name_vector.size() && !get_config_bool(OVERWRITE_FLUIDS)) {
        throw ValueError(format("Unable to load fluid [%s] due to error: Cannot load fluid [%s:%s] because it is already in library; "
                                "index = [%i] of [%i]; Consider enabling the config boolean variable OVERWRITE_FLUIDS",
                                fluid.name.c_str(), fluid.name.c_str(), fluid.CAS.c_str(), index, name_vector.size()));
    }
    const std::string name = fluid.name;
    register_identifiers(fluid, index);
    insert_fluid(fluid_json, std::move(fluid), index);
    if (index == name_vector.size()) name_vector.push_back(name);
}

void JSONFluidLibrary::load_index(const nlohmann::json& index, std::function<std::string(const std::string&)> provider) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // Only metadata is needed for FluidsList and identifier resolution.
    for (const auto& entry : index) {
        CoolPropFluid fluid;
        fluid.name = entry.at("NAME").get<std::string>();
        fluid.CAS = entry.at("CAS").get<std::string>();
        fluid.REFPROPname = entry.at("REFPROP_NAME").get<std::string>();
        fluid.aliases = cpjson::get_string_array(entry.at("ALIASES"));
        const auto slot = name_vector.size();
        register_identifiers(fluid, slot);
        pending.emplace(slot, entry.at("file").get<std::string>());
        name_vector.push_back(fluid.name);
    }
    data_provider = std::move(provider);
    _is_empty = false;
}

void JSONFluidLibrary::ensure_fluid_loaded(std::size_t index) {
    auto it = pending.find(index);
    if (it == pending.end()) return;
    const std::string name = name_vector.at(index);
    if (!loading.insert(index).second) {
        throw ValueError(format("Recursive fluid data provider request for fluid [%s]", name.c_str()));
    }
    try {
        const std::string bytes = data_provider(it->second);
        auto json = cpjson::from_cbor(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        CoolPropFluid fluid = parse_fluid(json);
        if (fluid.name != name) throw ValueError("Provider returned data for a different fluid");
        insert_fluid(json, std::move(fluid), index);
        loading.erase(index);
    } catch (const std::exception& e) {
        loading.erase(index);
        throw ValueError(format("Unable to load external fluid [%s]: %s", name.c_str(), e.what()));
    } catch (...) {
        loading.erase(index);
        throw ValueError(format("Unable to load external fluid [%s]: provider failed", name.c_str()));
    }
}

JSONFluidLibrary& get_library() {
    ensure_library_loaded();
    return library;
}

CoolPropFluid get_fluid(const std::string& fluid_string) {
    ensure_library_loaded();
    return library.get(fluid_string);
}

std::string get_fluid_as_JSONstring(const std::string& identifier) {
    ensure_library_loaded();
    return library.get_JSONstring(identifier);
}

std::string get_fluid_list() {
    ensure_library_loaded();
    return library.get_fluid_list();
};

void set_fluid_enthalpy_entropy_offset(const std::string& fluid, double delta_a1, double delta_a2, const std::string& ref) {
    ensure_library_loaded();
    library.set_fluid_enthalpy_entropy_offset(fluid, delta_a1, delta_a2, ref);
}

} /* namespace CoolProp */
