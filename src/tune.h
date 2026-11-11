/*
  Nest, a UCI/XBoard chess engine.
  Copyright (C) 2026 The Nest developers
  Distributed under the GNU General Public License v3, see LICENSE.
*/

#pragma once

// Search constants are declared with TUNABLE(name, value, min, max).
// In a normal build they are constexpr and cost nothing. With -DTUNE they
// become variables registered here and exposed as UCI spin options, which is
// what an SPSA tuner needs.

#if defined(TUNE)

    #include <vector>

namespace Nest {

struct TuneParam {
    const char* name;
    int*        value;
    int         def, min, max;
};

inline std::vector<TuneParam>& tune_registry() {
    static std::vector<TuneParam> registry;
    return registry;
}

struct TuneRegistrar {
    TuneRegistrar(const char* name, int* value, int min, int max) {
        tune_registry().push_back({name, value, *value, min, max});
    }
};

}  // namespace Nest

    #define TUNABLE(name, value, min, max) \
        inline int                 name = value; \
        inline Nest::TuneRegistrar name##_registrar(#name, &name, min, max);

#else

    #define TUNABLE(name, value, min, max) constexpr int name = value;

#endif
