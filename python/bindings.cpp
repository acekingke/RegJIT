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

// JIT function signature: int match(const char* input, int* start_out, int* end_out)
typedef int (*JitFunc)(const char*, int*, int*);

class PyRegex {
public:
    std::string pattern;
    uintptr_t func_ptr;  // Cached JIT function pointer
    
    PyRegex(const std::string &pat) : pattern(pat), func_ptr(0) {
        char* err = nullptr;
        if (!regjit_acquire(pat.c_str(), &err)) {
            std::string emsg = err ? std::string(err) : "acquire/compile failed";
            if (err) free(err);
            throw std::runtime_error(emsg);
        }
        // Cache the JIT function pointer for fast matching
        func_ptr = regjit_get_func_ptr(pat.c_str());
    }
    
    ~PyRegex() {
        regjit_release(pattern.c_str());
    }
    
    // Fast match using cached function pointer - no acquire/release overhead
    py::object match_str_fast(const std::string &s) {
        if (func_ptr == 0) {
            throw std::runtime_error("JIT function not available");
        }
        
        // Ensure null termination
        const char* cstr = s.c_str();
        
        JitFunc func = (JitFunc)func_ptr;
        int start = -1, end = -1;
        int matched = func(cstr, &start, &end);
        
        if (matched == 1) {
            return py::cast(PyMatch(start, end));
        }
        return py::none();
    }
    
    // Fast search using cached function pointer
    py::object search_str_fast(const std::string &s) {
        if (func_ptr == 0) {
            throw std::runtime_error("JIT function not available");
        }
        
        const char* cstr = s.c_str();
        
        JitFunc func = (JitFunc)func_ptr;
        int start = -1, end = -1;
        int matched = func(cstr, &start, &end);
        
        if (matched == 1) {
            return py::cast(PyMatch(start, end));
        }
        return py::none();
    }
    
    py::object match_bytes(py::bytes b) {
        std::string s = static_cast<std::string>(b);
        return match_str_fast(s);
    }
    
    py::object match_str(const std::string &s) {
        return match_str_fast(s);
    }
    
    py::object search_str(const std::string &s) {
        return search_str_fast(s);
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
