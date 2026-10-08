#include "tally/tally.hpp"

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <sstream>
#include <string>
#include <utility>

namespace py = pybind11;

namespace {

py::bytes as_bytes(const std::string& raw) {
    return py::bytes(raw.data(), static_cast<py::ssize_t>(raw.size()));
}

template <typename T>
void bind_snapshot(py::class_<T>& cls) {
    cls.def(py::self == py::self)
        .def("to_bytes", [](const T& self) { return as_bytes(self.serialize()); })
        .def_static(
            "from_bytes",
            [](py::bytes data) { return T::deserialize(data.cast<std::string>()); },
            py::arg("data"))
        .def("__copy__", [](const T& self) { return T(self); })
        .def(
            "__deepcopy__", [](const T& self, py::dict) { return T(self); }, py::arg("memo"))
        .def(py::pickle(
            [](const T& self) { return py::make_tuple(as_bytes(self.serialize())); },
            [](py::tuple state) {
                tally::detail::require(state.size() == 1, "invalid tally snapshot");
                return T::deserialize(state[0].cast<std::string>());
            }));
}

template <typename T>
void bind_key_update(py::class_<T>& cls) {
    cls.def(
        "update",
        [](T& self, py::iterable items) {
            for (py::handle item : items) {
                self.add(item.cast<std::string>());
            }
        },
        py::arg("items"),
        "Add each key once.");
}

}  // namespace

PYBIND11_MODULE(_tally, m) {
    m.doc() = "Streaming sketches. The hot path is C++; this module is the language boundary.";
    m.attr("__version__") = "0.1.0";

    py::class_<tally::ItemCount>(m, "ItemCount")
        .def_readonly("key", &tally::ItemCount::key)
        .def_readonly("count", &tally::ItemCount::count)
        .def_readonly("error", &tally::ItemCount::error)
        .def("__iter__",
             [](const tally::ItemCount& item) {
                 return py::iter(py::make_tuple(item.key, item.count, item.error));
             })
        .def("__repr__", [](const tally::ItemCount& item) {
            const std::string key = py::repr(py::cast(item.key));
            return "ItemCount(key=" + key + ", count=" + std::to_string(item.count) +
                   ", error=" + std::to_string(item.error) + ")";
        });

    auto bloom = py::class_<tally::BloomFilter>(m, "BloomFilter",
                                                 R"doc(Membership sketch.

A key passed to add() always reports as present. A key that was never added
sometimes reports as present. error_rate is the expected false-positive rate
once about capacity distinct keys have been inserted.)doc")
                      .def(py::init<std::size_t, double>(), py::arg("capacity"), py::arg("error_rate"))
                      .def("add", &tally::BloomFilter::add, py::arg("item"))
                      .def("might_contain", &tally::BloomFilter::might_contain, py::arg("item"))
                      .def("__contains__", &tally::BloomFilter::might_contain, py::arg("item"))
                      .def("merge", &tally::BloomFilter::merge, py::arg("other"),
                           "Bitwise OR of two filters with the same parameters.")
                      .def_property_readonly("capacity", &tally::BloomFilter::capacity)
                      .def_property_readonly("error_rate", &tally::BloomFilter::error_rate)
                      .def_property_readonly("bit_count", &tally::BloomFilter::bit_count)
                      .def_property_readonly("hash_count", &tally::BloomFilter::hash_count)
                      .def_property_readonly("fill_ratio", &tally::BloomFilter::fill_ratio)
                      .def_property_readonly("size_bytes", &tally::BloomFilter::size_bytes)
                      .def("__repr__", [](const tally::BloomFilter& filter) {
                          std::ostringstream out;
                          out << "<BloomFilter capacity=" << filter.capacity()
                              << " bits=" << filter.bit_count() << " hashes=" << filter.hash_count() << ">";
                          return out.str();
                      });
    bind_key_update(bloom);
    bind_snapshot(bloom);

    auto hll = py::class_<tally::HyperLogLog>(m, "HyperLogLog",
                                               R"doc(Distinct-count sketch.

precision 4..16 selects 2^precision bytes of state. estimate() is the
HyperLogLog estimator with the small-range linear-counting correction.)doc")
                    .def(py::init<int>(), py::arg("precision") = 14)
                    .def("add", &tally::HyperLogLog::add, py::arg("item"))
                    .def("merge", &tally::HyperLogLog::merge, py::arg("other"))
                    .def("estimate", &tally::HyperLogLog::estimate)
                    .def_property_readonly("precision", &tally::HyperLogLog::precision)
                    .def_property_readonly("size_bytes", &tally::HyperLogLog::size_bytes)
                    .def("__repr__", [](const tally::HyperLogLog& sketch) {
                        std::ostringstream out;
                        out << "<HyperLogLog precision=" << sketch.precision()
                            << " estimate=" << sketch.estimate() << ">";
                        return out.str();
                    });
    bind_key_update(hll);
    bind_snapshot(hll);

    auto cms =
        py::class_<tally::CountMinSketch>(m, "CountMinSketch",
                                           R"doc(Frequency sketch.

estimate(key) is always at least the true count. from_error(epsilon, delta)
sizes the table so the overestimate is at most epsilon times the total count
with probability at least 1 - delta. Sketches with the same shape merge by
adding counters.)doc")
            .def(py::init<std::size_t, std::size_t>(), py::arg("width"), py::arg("depth"))
            .def_static("from_error", &tally::CountMinSketch::from_error, py::arg("epsilon"), py::arg("delta"))
            .def("add", &tally::CountMinSketch::add, py::arg("item"), py::arg("count") = static_cast<std::uint64_t>(1))
            .def("estimate", &tally::CountMinSketch::estimate, py::arg("item"))
            .def("merge", &tally::CountMinSketch::merge, py::arg("other"))
            .def_property_readonly("width", &tally::CountMinSketch::width)
            .def_property_readonly("depth", &tally::CountMinSketch::depth)
            .def_property_readonly("size_bytes", &tally::CountMinSketch::size_bytes)
            .def("__repr__", [](const tally::CountMinSketch& sketch) {
                std::ostringstream out;
                out << "<CountMinSketch width=" << sketch.width() << " depth=" << sketch.depth() << ">";
                return out.str();
            });
    bind_key_update(cms);
    bind_snapshot(cms);

    auto reservoir =
        py::class_<tally::Reservoir>(m, "Reservoir",
                                     R"doc(Fixed-size uniform sample of a numeric stream.

quantile() interpolates the sample. The first capacity values are kept in
arrival order; later values replace them with Algorithm R.)doc")
            .def(py::init<std::size_t, std::uint64_t>(), py::arg("capacity"),
                 py::arg("seed") = static_cast<std::uint64_t>(0x5EED1234C0FFEEULL))
            .def("add", &tally::Reservoir::add, py::arg("value"))
            .def(
                "update",
                [](tally::Reservoir& self, py::iterable values) {
                    for (py::handle value : values) {
                        self.add(value.cast<double>());
                    }
                },
                py::arg("values"))
            .def("quantile", &tally::Reservoir::quantile, py::arg("q"))
            .def("sample", &tally::Reservoir::sample)
            .def_property_readonly("capacity", &tally::Reservoir::capacity)
            .def_property_readonly("size", &tally::Reservoir::size)
            .def_property_readonly("seen", &tally::Reservoir::seen)
            .def_property_readonly("size_bytes", &tally::Reservoir::size_bytes)
            .def("__repr__", [](const tally::Reservoir& sample) {
                std::ostringstream out;
                out << "<Reservoir capacity=" << sample.capacity() << " seen=" << sample.seen() << ">";
                return out.str();
            });
    bind_snapshot(reservoir);

    auto heavy =
        py::class_<tally::SpaceSaving>(m, "SpaceSaving",
                                        R"doc(Heavy-hitter summary.

Keeps k counters. For a monitored key the true count is in
[count - error, count]. upper_bound() covers keys that have already been
evicted: their true count is at most the smallest counter.)doc")
            .def(py::init<std::size_t>(), py::arg("k"))
            .def("add", &tally::SpaceSaving::add, py::arg("item"), py::arg("count") = static_cast<std::uint64_t>(1))
            .def("monitored", &tally::SpaceSaving::monitored, py::arg("item"))
            .def("upper_bound", &tally::SpaceSaving::upper_bound, py::arg("item"))
            .def("lower_bound", &tally::SpaceSaving::lower_bound, py::arg("item"))
            .def(
                "top",
                [](const tally::SpaceSaving& self, py::object limit) {
                    auto rows = self.top();
                    if (!limit.is_none()) {
                        const auto n = limit.cast<std::size_t>();
                        if (n < rows.size()) {
                            rows.resize(n);
                        }
                    }
                    return rows;
                },
                py::arg("n") = py::none())
            .def_property_readonly("k", &tally::SpaceSaving::k)
            .def_property_readonly("size", &tally::SpaceSaving::size)
            .def_property_readonly("size_bytes", &tally::SpaceSaving::size_bytes)
            .def("__repr__", [](const tally::SpaceSaving& summary) {
                std::ostringstream out;
                out << "<SpaceSaving k=" << summary.k() << " size=" << summary.size() << ">";
                return out.str();
            });
    bind_key_update(heavy);
    bind_snapshot(heavy);
}
