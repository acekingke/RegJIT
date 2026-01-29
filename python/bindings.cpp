#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>
#include <cstdlib>
#include "../src/regjit_capi.h"

namespace py = pybind11;

class PyRegex {
public:
    std::string pattern;
    PyRegex(const std::string &pat) : pattern(pat) {
        char* err = nullptr;
        if (!regjit_compile(pat.c_str(), &err)) {
            std::string emsg = err ? std::string(err) : "compile failed";
            if (err) free(err);
            throw std::runtime_error(emsg);
        }
    }
    ~PyRegex() {
        regjit_unload(pattern.c_str());
    }

    bool match_bytes(py::bytes b) {
        std::string s = static_cast<std::string>(b);
        int r = regjit_match(pattern.c_str(), s.data(), s.size());
        if (r < 0) throw std::runtime_error("match error");
        return r == 1;
    }

    bool match_str(const std::string &s) {
        // encode to UTF-8 bytes (std::string already holds UTF-8 in Python3)
        int r = regjit_match(pattern.c_str(), s.data(), s.size());
        if (r < 0) throw std::runtime_error("match error");
        return r == 1;
    }
};

PYBIND11_MODULE(_regjit, m) {
    py::class_<PyRegex>(m, "Regex")
        .def(py::init<const std::string&>())
        .def("match_bytes", &PyRegex::match_bytes)
        .def("match", (bool (PyRegex::*)(const std::string&)) &PyRegex::match_str)
        ;

    m.def("compile", [](const std::string &pat){ return PyRegex(pat); });
}
