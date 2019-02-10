/*
    src/autodiff/autodiff.cpp -- Reverse mode automatic differentiation

    Enoki is a C++ template library that enables transparent vectorization
    of numerical kernels using SIMD instruction sets available on current
    processor architectures.

    Copyrighe (c) 2018 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#include <enoki/dynamic.h>

#if ENOKI_BUILD_CUDA
#  include <enoki/cuda.h>
#endif

#include <enoki/autodiff.h>

#include <unordered_map>
#include <set>
#include <sstream>

#if defined(NDEBUG)
#  define ENOKI_AUTODIFF_DEFAULT_LOG_LEVEL 0
#else
#  define ENOKI_AUTODIFF_DEFAULT_LOG_LEVEL 1
#endif

NAMESPACE_BEGIN(enoki)

using Index = uint32_t;

template <typename Value>
Value safe_mul(const Value &value1, const Value &value2);
template <typename Value>
Value safe_fmadd(const Value &value1, const Value &value2, const Value &value3);

template <typename Value> struct Tape<Value>::Node {
    /// Descriptive label
    std::string label;

    /// Gradient value
    Value grad;

    /// Pointer to incident edge linked list
    std::unique_ptr<Edge> edges;

    /// External (i.e. by Enoki) reference count
    uint32_t ref_count = 0;

    /// Size of the variable
    uint32_t size;

    Node(size_t size, const char *label)
        : label(label ? label : ""), size((uint32_t) size) { }

    bool is_scalar() const {
        return size == 1;
    }

    Index degree() const {
        Index result = 0;
        Edge *edge = edges.get();
        while (edge) {
            edge = edge->next.get();
            result += 1;
        }
        return result;
    }

    bool has_special() const {
        bool result = false;
        Edge *edge = edges.get();
        while (edge) {
            result |= edge->is_special();
            edge = edge->next.get();
        }
        return result;
    }

    void append_edge(Edge *edge) {
        if (edges.get() == nullptr) {
            edges = std::unique_ptr<Edge>(edge);
        } else {
            Edge *cur = edges.get();
            while (cur->next.get())
                cur = cur->next.get();
            cur->next = std::unique_ptr<Edge>(edge);
        }
    }
};

template <typename Value> struct Tape<Value>::Edge {
    /// Source node ID associated with this edge
    Index source;

    /// Edge weight
    Value weight;

    /// Optional: special operation (scatter/gather/reduction)
    std::unique_ptr<Special> special;

    /// Pointer to next edge
    std::unique_ptr<Edge> next;

    Edge(Index source, const Value &weight)
        : source(source), weight(weight) { }

    Edge(Index source, Special *special)
        : source(source), special(special) { }

    bool is_special() const { return special != nullptr; }
};

template <typename Value> struct Tape<Value>::Special {
    virtual void compute_gradients(Detail *detail, Index target,
                                   const Edge &edge) const = 0;
    virtual ~Special() = default;
};

template <typename Value> struct Tape<Value>::Detail {
    Index node_counter = 1,
          node_counter_last = 1,
          edge_contractions = 0,
          edge_contractions_last = 0,
          edge_merges = 0,
          edge_merges_last = 0;

    std::unordered_map<Index, Node> nodes;
    std::vector<std::string> prefix;
    Index *scatter_gather_index = nullptr;
    size_t scatter_gather_size = 0;
    bool scatter_gather_permute = false;
    uint32_t log_level = ENOKI_AUTODIFF_DEFAULT_LOG_LEVEL;
    bool contract_edges = true;

    /// Set of indices selected for next backward pass
    std::set<uint32_t> scheduled;

    Node &node(Index index) {
        auto it = nodes.find(index);
        if (it == nodes.end())
            throw std::runtime_error("autodiff: Detail::node(): Unknown index " +
                                     std::to_string(index));
        return it->second;
    }

    void dfs(Index k, bool clear_grad) {
        if (scheduled.find(k) != scheduled.end())
            return;
        scheduled.insert(k);

        Node &n = node(k);
        if (clear_grad) {
            n.grad = zero<Value>(n.size);
            if (!n.label.empty())
                enoki::set_label(n.grad, (n.label + ".grad").c_str());
        }
        const Edge *edge = n.edges.get();
        while (edge) {
            dfs(edge->source, clear_grad);
            edge = edge->next.get();
        }
    }
};

template <typename Value> Tape<Value> Tape<Value>::s_tape;
template <typename Value> Tape<Value> *Tape<Value>::get() { return &s_tape; }

template <typename Value> Tape<Value>::Tape() {
    d = new Detail();
}

template <typename Value> Tape<Value>::~Tape() {
#if !defined(NDEBUG)
    if (d->log_level >= 1) {
        for (const auto &it : d->nodes) {
            std::cerr << "autodiff: variable " << it.first
                      << " still live at shutdown. (ref_count="
                      << it.second.ref_count << ")" << std::endl;
        }
    }
#endif
    delete d;
}

template <typename Value> void Tape<Value>::set_log_level(uint32_t level) {
    d->log_level = level;
}

template <typename Value> void Tape<Value>::set_contract_edges(bool value) {
    d->contract_edges = value;
}

template <typename Value>
Index Tape<Value>::append(const char *label, size_t size, Index i1, const Value &w1) {
    if (i1 == 0)
        return 0;
    Index idx = append_node(size, label);
#if !defined(NDEBUG)
    if (d->log_level >= 3)
        std::cerr << "autodiff: append(\"" << (label ? label : "") << "\", " << idx
                  << " <- " << i1 << ")" << std::endl;
#endif
    append_edge(i1, idx, w1);
    return idx;
}

template <typename Value>
Index Tape<Value>::append(const char *label, size_t size, Index i1, Index i2,
                          const Value &w1, const Value &w2) {
    if (i1 == 0 && i2 == 0)
        return 0;
    Index idx = append_node(size, label);
#if !defined(NDEBUG)
    if (d->log_level >= 3)
        std::cerr << "autodiff: append(\"" << (label ? label : "") << "\", " << idx
                  << " <- [" << i1 << ", " << i2 << "])" << std::endl;
#endif
    append_edge(i1, idx, w1);
    append_edge(i2, idx, w2);
    return idx;
}

template <typename Value>
Index Tape<Value>::append(const char *label, size_t size, Index i1, Index i2, Index i3,
                          const Value &w1, const Value &w2, const Value &w3) {
    if (i1 == 0 && i2 == 0 && i3 == 0)
        return 0;
    Index idx = append_node(size, label);
#if !defined(NDEBUG)
    if (d->log_level >= 3)
        std::cerr << "autodiff: append(\"" << (label ? label : "") << "\", " << idx
                  << " <- [" << i1 << ", " << i2 << ", " << i3 << "])" << std::endl;
#endif
    append_edge(i1, idx, w1);
    append_edge(i2, idx, w2);
    append_edge(i3, idx, w3);
    return idx;
}

template <typename Value>
Index Tape<Value>::append_node(size_t size, const char *label) {
    Index idx = d->node_counter++;
    auto result = d->nodes.emplace(std::make_pair(idx, Node(size, label)));

    Node &node = result.first->second;
    for (auto it = d->prefix.rbegin(); it != d->prefix.rend(); ++it)
        node.label = *it + '/' + node.label;

#if !defined(NDEBUG)
    if (d->log_level >= 3)
        std::cerr << "autodiff: append_node(\"" << (label ? label : "")
                  << "\", size=" << size << ") -> " << idx << std::endl;
#endif
    inc_ref(idx);
    return idx;
}

template <typename Value>
Index Tape<Value>::append_leaf(size_t size) {
    Index idx = append_node(size, "'unnamed'");
    Node &n = d->node(idx);
    n.grad = zero<Value>(n.size);
    return idx;
}

template <typename Value>
void Tape<Value>::set_label(Index idx, const char *label) {
    if (idx == 0)
        return;
#if !defined(NDEBUG)
    if (d->log_level >= 3)
        std::cerr << "autodiff: set_label(" << idx << ") -> " << label << std::endl;
#endif
    std::string name = "'" + std::string(label) + "'";
    Node &n = d->node(idx);
    n.label = name;
    enoki::set_label(n.grad, (label + std::string(".grad")).c_str());
}


template <typename Value>
Index Tape<Value>::append_gather(const Int64 &offset, const Mask &mask) {
    if constexpr (is_dynamic_v<Value>) {
        if (d->scatter_gather_index == nullptr ||
           *d->scatter_gather_index == 0)
            return 0;
        Index source = *d->scatter_gather_index;

        struct Gather : Special {
            Int64 offset;
            Mask mask;
            size_t size;
            bool permute;

            void compute_gradients(Detail *detail, Index target,
                                   const Edge &edge) const override {
                const Value &grad_target = detail->node(target).grad;
                Value &grad_source = detail->node(edge.source).grad;
                assert(grad_source.size() == size);

                if (permute)
                    scatter(grad_source, grad_target, offset, mask);
                else
                    scatter_add(grad_source, grad_target, offset, mask);
            }
        };

        Gather *gather = new Gather();
        gather->offset = offset;
        gather->mask = mask;
        gather->size = d->node(source).size;
        gather->permute = d->scatter_gather_permute;

        Index target = append_node(slices(offset), "gather");
        d->node(target).append_edge(new Edge(source, gather));
        inc_ref(source);

#if !defined(NDEBUG)
        if (d->log_level >= 3)
            std::cerr << "autodiff: append_gather(" << target << " <- " << source << ")"
                      << std::endl;
#endif

        return target;
    } else {
        return 0;
    }
}

template <typename Value>
void Tape<Value>::append_scatter(Index source, const Int64 &offset, const Mask &mask) {
    if constexpr (is_dynamic_v<Value>) {
        if (d->scatter_gather_index == nullptr)
            return;
        Index target_orig = *d->scatter_gather_index;

        struct Scatter : Special {
            Int64 offset;
            Mask mask;

            void compute_gradients(Detail *detail, Index target,
                                   const Edge &edge) const override {
                const Value &grad_target = detail->node(target).grad;
                Value &grad_source = detail->node(edge.source).grad;
                grad_source += gather<Value>(grad_target, offset, mask);
            }
        };

        Scatter *s = new Scatter();
        s->offset = offset;
        s->mask = mask;

        Index target_new = append_node(d->scatter_gather_size, "scatter");
        d->node(target_new).append_edge(new Edge(source, s));
        inc_ref(source);

        if (target_orig != 0) {
            Index sa_node = target_new;
            Value weight = 1.f;
            if (!d->scatter_gather_permute) {
                weight = full<Value>(1.f, d->scatter_gather_size);
                scatter(weight, Value(0), offset, mask);
            }
            target_new = append("scatter_combine", d->scatter_gather_size,
                                target_new, target_orig, 1, weight);
            dec_ref(sa_node);
            dec_ref(target_orig);
        }

        *d->scatter_gather_index = target_new;

#if !defined(NDEBUG)
        if (d->log_level >= 3)
            std::cerr << "autodiff: append_scatter(" << target_orig << " <- "
                      << source << ") -> " << target_new << std::endl;
#endif
    }
}

template <typename Value>
void Tape<Value>::append_scatter_add(Index source, const Int64 &offset,
                                     const Mask &mask) {
    if constexpr (is_dynamic_v<Value>) {
        if (d->scatter_gather_index == nullptr)
            return;
        Index target_orig = *d->scatter_gather_index;

        struct Scatter : Special {
            Int64 offset;
            Mask mask;

            void compute_gradients(Detail *detail, Index target,
                                   const Edge &edge) const override {
                const Value &grad_target = detail->node(target).grad;
                Value &grad_source = detail->node(edge.source).grad;
                grad_source += gather<Value>(grad_target, offset, mask);
            }
        };

        Scatter *s = new Scatter();
        s->offset = offset;
        s->mask = mask;

        Index target_new = append_node(d->scatter_gather_size, "scatter_add");
        d->node(target_new).append_edge(new Edge(source, s));
        inc_ref(source);

        if (target_orig != 0) {
            Index sa_node = target_new;
            target_new = append("add", d->scatter_gather_size, target_new,
                                target_orig, 1, 1);
            dec_ref(sa_node);
            dec_ref(target_orig);
        }

        *d->scatter_gather_index = target_new;

#if !defined(NDEBUG)
        if (d->log_level >= 3)
            std::cerr << "autodiff: append_scatter_add(" << target_orig << " <- "
                      << source << ") -> " << target_new << std::endl;
#endif
    }
}

template <typename Value>
void Tape<Value>::append_edge(Index source_idx, Index target_idx,
                              const Value &weight) {
    using Scalar = scalar_t<Value>;
    if (source_idx == 0)
        return;
    assert(target_idx != 0);

#if !defined(NDEBUG)
    if (d->log_level >= 4)
        std::cerr << "autodiff: append_edge(" << target_idx << " <- " << source_idx << ")" << std::endl;
#endif

    Node &source = d->node(source_idx),
         &target = d->node(target_idx);

    Index source_deg = source.degree();
    bool has_special = source.has_special();
    bool compat_size = source.size == target.size;

    if (!has_special && compat_size && source_deg > 0 && d->contract_edges) {
        Edge *edge = source.edges.get();
        while (edge) {
#if !defined(NDEBUG)
            if (d->log_level >= 4)
                std::cerr << " ... contracting with edge -> "  << edge->source << std::endl;
#endif
            append_edge_prod(edge->source, target_idx, weight, edge->weight);
            d->edge_contractions++;
            edge = edge->next.get();
        }
        return;
    }

    Edge *edge = target.edges.get();
    while (edge) {
        if (edge->source == source_idx) {

            edge->weight += weight;
#if !defined(NDEBUG)
            if (d->log_level >= 4) {
                std::cerr << " ... merging into existing edge" << std::endl;

                enoki::set_label(edge->weight,
                                 ("edge[" + std::to_string(source_idx) + " -> " +
                                  std::to_string(target_idx) + "]").c_str());
            }
#endif
            d->edge_merges++;
            return;
        }
        edge = edge->next.get();
    }

#if !defined(NDEBUG)
    if (d->log_level >= 4)
        enoki::set_label(weight, ("edge[" + std::to_string(source_idx) + " -> " +
                                  std::to_string(target_idx) + "]").c_str());
#endif

    target.append_edge(new Edge(source_idx, weight));
    inc_ref(source_idx);
}

template <typename Value>
void Tape<Value>::append_edge_prod(Index source_idx, Index target_idx,
                                   const Value &weight1, const Value &weight2) {
    using Scalar = scalar_t<Value>;
    if (source_idx == 0)
        return;
    assert(target_idx != 0);

#if !defined(NDEBUG)
    if (d->log_level >= 4)
        std::cerr << "autodiff: append_edge(" << target_idx << " <- " << source_idx << ")" << std::endl;
#endif

    Node &source = d->node(source_idx),
         &target = d->node(target_idx);

    Index source_deg = source.degree();
    bool has_special = source.has_special();
    bool compat_size = source.size == target.size;

    if (!has_special && compat_size && source_deg > 0 && d->contract_edges) {
        Edge *edge = source.edges.get();
        while (edge) {
#if !defined(NDEBUG)
            if (d->log_level >= 4)
                std::cerr << " ... contracting with edge -> "  << edge->source << std::endl;
#endif
            append_edge_prod(edge->source, target_idx,
                             safe_mul(weight1, weight2), edge->weight);
            d->edge_contractions++;
            edge = edge->next.get();
        }
        return;
    }

    Edge *edge = target.edges.get();
    while (edge) {
        if (edge->source == source_idx) {
            edge->weight = safe_fmadd(weight1, weight2, edge->weight);

#if !defined(NDEBUG)
            if (d->log_level >= 4) {
                std::cerr << " ... merging into existing edge" << std::endl;
                enoki::set_label(edge->weight,
                                 ("edge[" + std::to_string(source_idx) + " -> " +
                                  std::to_string(target_idx) + "]").c_str());
            }
#endif

            d->edge_merges++;
            return;
        }
        edge = edge->next.get();
    }

    Value weight = safe_mul(weight1, weight2);
#if !defined(NDEBUG)
    if (d->log_level >= 4)
        enoki::set_label(weight, ("edge[" + std::to_string(source_idx) + " -> " +
                                  std::to_string(target_idx) + "]").c_str());
#endif

    target.append_edge(new Edge(source_idx, weight));
    inc_ref(source_idx);
}


template <typename Value> void Tape<Value>::inc_ref(Index index) {
    if (index == 0)
        return;
    Node &node = d->node(index);
    node.ref_count++;
#if !defined(NDEBUG)
    if (d->log_level >= 4)
        std::cerr << "autodiff: inc_ref(" << index << ") -> " << node.ref_count << std::endl;
#endif
}

template <typename Value> void Tape<Value>::dec_ref(Index index) {
    if (index == 0)
        return;
    Node &node = d->node(index);
    if (node.ref_count == 0)
        throw std::runtime_error("autodiff: dec_ref(): Node " +
                                 std::to_string(index) +
                                 " has zero references!");
    --node.ref_count;
#if !defined(NDEBUG)
    if (d->log_level >= 4)
        std::cerr << "autodiff: dec_ref(" << index << ") -> " << node.ref_count << std::endl;
#endif
    if (node.ref_count == 0)
        free_node(index);
}

template <typename Value> void Tape<Value>::free_node(Index index) {
#if !defined(NDEBUG)
    if (d->log_level >= 4)
        std::cerr << "autodiff: free_node(" << index << ")" << std::endl;
#endif

    auto it = d->nodes.find(index);
    if (it == d->nodes.end())
        throw std::runtime_error("autodiff: free_node(): Unknown index " +
                                 std::to_string(index));
    Node &node = it->second;
    Edge *edge = node.edges.get();

    while (edge) {
        dec_ref(edge->source);
        edge = edge->next.get();
    }

    d->nodes.erase(it);
}

template <typename Value> void Tape<Value>::push_prefix(const char *value) {
    d->prefix.push_back(value);
}

template <typename Value> void Tape<Value>::pop_prefix() {
    if (d->prefix.empty())
        throw std::runtime_error("pop_prefix(): prefix list is already empty!");
    d->prefix.pop_back();
}

template <typename Value>
void Tape<Value>::set_scatter_gather_operand(Index *index, size_t size,
                                             bool permute) {
    d->scatter_gather_index = index;
    d->scatter_gather_size = size;
    d->scatter_gather_permute = permute;
}

template <typename Value> const Value &Tape<Value>::gradient(Index index) {
    if (index == 0)
        throw std::runtime_error(
            "No gradient was computed for this variable! (a call to "
            "requires_gradient() is necessary.)");
    return d->node(index).grad;
}

template <typename Value>
void Tape<Value>::set_gradient(Index index, const Value &value) {
    if (index == 0)
        throw std::runtime_error(
            "backward(): no gradient information (a prior call to "
            "requires_gradient() on a dependent variable is required.)");

    d->dfs(index, true);
    d->node(index).grad = value;
}

template <typename Value>
void Tape<Value>::backward(bool free_graph) {
    using Scalar = scalar_t<Value>;
    auto &scheduled = d->scheduled;
    uint32_t edge_count = 0;

    if (free_graph) {
        for (auto it = scheduled.rbegin(); it != scheduled.rend(); ++it)
            inc_ref(*it);
    }

    for (auto it = scheduled.rbegin(); it != scheduled.rend(); ++it) {
        Index target_idx = *it;
        Node &target = d->node(target_idx);

        if constexpr (is_dynamic_v<Value>) {
            if (ENOKI_UNLIKELY(target.is_scalar() && target.grad.size() != 1))
                target.grad = hsum(target.grad);
        }

        Edge *edge = target.edges.get();
        while (edge) {
            Index source_idx = edge->source;

            if (ENOKI_LIKELY(!edge->is_special())) {
                Value &weight = edge->weight;
                Node  &source = d->node(source_idx);

                source.grad = safe_fmadd(weight, target.grad, source.grad);

                ++edge_count;
            } else {
                edge->special->compute_gradients(d, target_idx, *edge);
                if (free_graph)
                    edge->special.reset();
            }
            if (free_graph)
                dec_ref(edge->source);
            edge = edge->next.get();
        }
        if (free_graph) {
            target.edges.reset();
            dec_ref(target_idx);
        }
    }

    if (d->log_level >= 1)
        std::cerr << "autodiff: processed " << scheduled.size() << "/" << (d->node_counter - d->node_counter_last)
                  << " nodes, " << edge_count << " edges [" << (d->edge_contractions - d->edge_contractions_last)
                  << " edge contractions, " << (d->edge_merges - d->edge_merges_last) << " edge merges].. "
                  << std::endl;

    if (free_graph) {
        d->node_counter_last = d->node_counter;
        d->edge_contractions_last = d->edge_contractions;
        d->edge_merges_last = d->edge_merges;
    }

    scheduled.clear();
}

template <typename Value>
std::string Tape<Value>::graphviz(const std::vector<Index> &indices_) {
    std::ostringstream oss;
    oss << "digraph {" << std::endl
        << "  rankdir=BT;" << std::endl // BT or RL
        << "  fontname=Consolas;" << std::endl
        << "  node [shape=record fontname=Consolas];" << std::endl;

    for (Index index : indices_)
        d->dfs(index, false);

    auto &indices = d->scheduled;

    int current_depth = 0;
    auto hasher = std::hash<std::string>();
    std::string current_path = "";

    for (Index index : indices) {
        const Node &node = d->node(index);
        if (node.label.empty())
            continue;
        std::string label = node.label;

        auto sepidx = label.rfind("/");
        std::string path, suffix;
        if (sepidx != std::string::npos) {
            path = label.substr(0, sepidx);
            label = label.substr(sepidx + 1);
        }

        if (current_path != path) {
            for (int i = 0; i < current_depth; ++i)
                oss << "  }" << std::endl;
            current_depth = 0;
            current_path = path;

            do {
                sepidx = path.find('/');
                std::string graph_label = path.substr(0, sepidx);
                if (graph_label.empty())
                    break;

                oss << "  subgraph cluster"
                    << std::to_string(hasher(graph_label)) << " {" << std::endl
                    << "  label=\"" << graph_label << "\";" << std::endl;
                ++current_depth;

                if (sepidx == std::string::npos)
                    break;
                path = path.substr(sepidx + 1, std::string::npos);
            } while (true);
        }

        oss <<  "  " << std::to_string(index) << " [label=\"" + label;
        if (node.is_scalar())
            oss << " [s]";

        oss << "\\n#" << std::to_string(index) << " ["
            << std::to_string(node.ref_count) << "]"
            << "\"";
        if (node.label[0] == '\'')
            oss << " fillcolor=salmon style=filled";
        oss << "];" << std::endl;
    }
    for (int i = 0; i < current_depth; ++i)
        oss << "  }\n";

    for (Index index : indices) {
        const Node &node = d->node(index);
        const Edge *edge = node.edges.get();

        while (edge) {
            oss << "  " << std::to_string(index) << " -> "
                << std::to_string(edge->source) << ";" << std::endl;

            if (edge->is_special())
                oss << "  " << std::to_string(index)
                    << " [shape=doubleoctagon];" << std::endl;
            edge = edge->next.get();
        }
    }

    for (Index idx : indices_)
        oss << "  " << std::to_string(idx)
            << " [fillcolor=cornflowerblue style=filled];" << std::endl;

    oss << "}";
    indices.clear();
    return oss.str();
}

template <typename Value> Value safe_mul(const Value &value1, const Value &value2) {
    Value tentative = value1 * value2;
    if constexpr (!is_cuda_array_v<Value>) {
        Value zero = scalar_t<Value>(0);
        mask_t<Value> is_zero = eq(value1, zero) || eq(value2, zero);
        return select(is_zero, zero, tentative);
    } else {
        using Mask = mask_t<Value>;
        Mask m1 = Mask::from_index_(cuda_trace_append(EnokiType::Bool, "setp.eq.f32 $r1, $r2, 0.0", value1.index_())),
             m2 = Mask::from_index_(cuda_trace_append(EnokiType::Bool, "setp.eq.or.f32 $r1, $r2, 0.0, $r3", value2.index_(), m1.index_()));
        return Value::from_index_(cuda_trace_append(Value::Type, "selp.$t1 $r1, 0.0, $r2, $r3", tentative.index_(), m2.index_()));
    }
}

template <typename Value> Value safe_fmadd(const Value &value1, const Value &value2, const Value &value3) {
    Value tentative = fmadd(value1, value2, value3);
    if constexpr (!is_cuda_array_v<Value>) {
        Value zero = scalar_t<Value>(0);
        mask_t<Value> is_zero = eq(value1, zero) || eq(value2, zero);
        return select(is_zero, value3, tentative);
    } else {
        using Mask = mask_t<Value>;
        Mask m1 = Mask::from_index_(cuda_trace_append(EnokiType::Bool, "setp.eq.f32 $r1, $r2, 0.0", value1.index_())),
             m2 = Mask::from_index_(cuda_trace_append(EnokiType::Bool, "setp.eq.or.f32 $r1, $r2, 0.0, $r3", value2.index_(), m1.index_()));
        return Value::from_index_(cuda_trace_append(Value::Type, "selp.$t1 $r1, $r2, $r3, $r4", value3.index_(), tentative.index_(), m2.index_()));
    }
}

template struct ENOKI_EXPORT Tape<float>;
template struct ENOKI_EXPORT DiffArray<float>;

template struct ENOKI_EXPORT Tape<double>;
template struct ENOKI_EXPORT DiffArray<double>;

template struct ENOKI_EXPORT Tape<DynamicArray<Packet<float>>>;
template struct ENOKI_EXPORT DiffArray<DynamicArray<Packet<float>>>;

template struct ENOKI_EXPORT Tape<DynamicArray<Packet<double>>>;
template struct ENOKI_EXPORT DiffArray<DynamicArray<Packet<double>>>;

#if ENOKI_BUILD_CUDA
template struct ENOKI_EXPORT Tape<CUDAArray<float>>;
template struct ENOKI_EXPORT DiffArray<CUDAArray<float>>;

template struct ENOKI_EXPORT Tape<CUDAArray<double>>;
template struct ENOKI_EXPORT DiffArray<CUDAArray<double>>;
#endif

NAMESPACE_END(enoki)