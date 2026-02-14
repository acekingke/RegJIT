#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>
#include <cstdlib>
#include "../src/regjit_capi.h"

namespace py = pybind11;

class PyMatch {
public:
    int m_start;
    int m_end;
    
    PyMatch(int start, int end) : m_start(start), m_end(end) {}
    
    int start() const { return m_start; }
    int end() const { return m_end; }
    std::pair<int, int> span() const { return {m_start, m_end}; }
    
    // Support truthiness check
    bool __bool__() const { return true; }
    
    std::string __repr__() const {
        return "<_regjit.Match object; span=(" + std::to_string(m_start) + ", " + std::to_string(m_end) + ")>";
    }
};

class PyRegex {
public:
    std::string pattern;
    PyRegex(const std::string &pat) : pattern(pat) {
        char* err = nullptr;
        if (!regjit_acquire(pat.c_str(), &err)) {
            std::string emsg = err ? std::string(err) : "acquire/compile failed";
            if (err) free(err);
            throw std::runtime_error(emsg);
        }
    }
    ~PyRegex() {
        regjit_release(pattern.c_str());
    }

    py::object match_bytes(py::bytes b) {
        std::string s = static_cast<std::string>(b);
        regjit_match_result r = regjit_match_at_start(pattern.c_str(), s.data(), s.size());
        if (r.matched < 0) throw std::runtime_error("match error");
        if (r.matched == 1) {
            return py::cast(PyMatch(r.start, r.end));
        }
        return py::none();
    }

    py::object match_str(const std::string &s) {
        // encode to UTF-8 bytes (std::string already holds UTF-8 in Python3)
        regjit_match_result r = regjit_match_at_start(pattern.c_str(), s.data(), s.size());
        if (r.matched < 0) throw std::runtime_error("match error");
        if (r.matched == 1) {
            return py::cast(PyMatch(r.start, r.end));
        }
        return py::none();
    }

    py::object search_str(const std::string &s) {
        regjit_match_result r = regjit_search(pattern.c_str(), s.data(), s.size());
        if (r.matched < 0) throw std::runtime_error("search error");
        if (r.matched == 1) {
            return py::cast(PyMatch(r.start, r.end));
        }
        return py::none();
    }
};

    PYBIND11_MODULE(_regjit, m) {
        py::class_<PyMatch>(m, "Match")
            .def("start", &PyMatch::start)
            .def("end", &PyMatch::end)
            .def("span", &PyMatch::span)
            .def("__bool__", &PyMatch::__bool__)
            .def("__repr__", &PyMatch::__repr__)
            ;

        py::class_<PyRegex>(m, "Regex")
            .def(py::init<const std::string&>())
            .def("match_bytes", &PyRegex::match_bytes)
            .def("match", (py::object (PyRegex::*)(const std::string&)) &PyRegex::match_str)
            .def("search", (py::object (PyRegex::*)(const std::string&)) &PyRegex::search_str)
            .def("unload", [](PyRegex &r){ regjit_unload(r.pattern.c_str()); })
            ;

    m.def("compile", [](const std::string &pat){ return PyRegex(pat); });
    m.def("cache_size", [](){ return regjit_cache_size(); });
    m.def("set_cache_maxsize", [](size_t n){ regjit_set_cache_maxsize(n); });
    m.def("acquire", [](const std::string &pat){ char* err = nullptr; if (!regjit_acquire(pat.c_str(), &err)) { std::string emsg = err ? std::string(err) : "acquire failed"; if (err) free(err); throw std::runtime_error(emsg); } });
    m.def("release", [](const std::string &pat){ regjit_release(pat.c_str()); });
    }
