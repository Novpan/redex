/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodOverrideGraph.h"

#include <boost/range/adaptor/map.hpp>

#include "BinarySerialization.h"
#include "PatriciaTreeMap.h"
#include "PatriciaTreeSet.h"
#include "Timer.h"
#include "Walkers.h"

using namespace method_override_graph;

namespace {

using MethodSet = sparta::PatriciaTreeSet<const DexMethod*>;

using ProtoMap = sparta::PatriciaTreeMap<const DexProto*, MethodSet>;

// The set of methods in scope at a particular class. We use PatriciaTreeMaps
// for this because there is a lot shared structure: the maps of subclasses /
// subinterfaces contain many elements from their parent classes / interfaces.
// We also do a lot of unioning operations when analyzing interfaces, and
// PatriciaTreeMaps are well-optimized for that.
using SignatureMap =
    sparta::PatriciaTreeMap<const DexString* /* name */, ProtoMap>;

struct ClassSignatureMap {
  // The methods implemented by the current class or one of its superclasses.
  // The MethodSets here should always be singleton sets.
  SignatureMap implemented;
  // The interface methods not yet implemented by the current class or its
  // superclasses.
  // The MethodSets here can have multiple elements -- a class can implement
  // multiple interfaces where some or all of them define a method with the
  // same signature.
  SignatureMap unimplemented;
};

using ClassSignatureMaps = ConcurrentMap<const DexClass*, ClassSignatureMap>;

using InterfaceSignatureMaps = ConcurrentMap<const DexClass*, SignatureMap>;

void update_signature_map(const DexMethod* method,
                          MethodSet value,
                          SignatureMap* map) {
  map->update(
      [&](const ProtoMap& protos) {
        auto copy = protos;
        copy.insert_or_assign(method->get_proto(), value);
        return copy;
      },
      method->get_name());
}

void unify_signature_maps(const SignatureMap& to_add, SignatureMap* target) {
  target->union_with(
      [&](const ProtoMap& p1, const ProtoMap& p2) {
        return p1.get_union_with(
            [&](const MethodSet& ms1, const MethodSet& ms2) {
              return ms1.get_union_with(ms2);
            },
            p2);
      },
      to_add);
}

class GraphBuilder {
 public:
  explicit GraphBuilder(const Scope& scope) : m_scope(scope) {}

  std::unique_ptr<Graph> run() {
    m_graph = std::make_unique<Graph>();
    walk::parallel::classes(m_scope, [&](const DexClass* cls) {
      if (is_interface(cls)) {
        analyze_interface(cls);
      } else {
        analyze_non_interface(cls);
      }
    });
    return std::move(m_graph);
  }

 private:
  ClassSignatureMap analyze_non_interface(const DexClass* cls) {
    always_assert(!is_interface(cls));
    if (m_class_signature_maps.count(cls) != 0) {
      return m_class_signature_maps.at(cls);
    }

    // Initialize the signature maps from those of the superclass.
    ClassSignatureMap class_signatures;
    if (cls->get_super_class() != nullptr) {
      auto super_cls = type_class(cls->get_super_class());
      if (super_cls != nullptr) {
        class_signatures = analyze_non_interface(super_cls);
      }
    }

    // Add all methods from the interfaces that the current class directly
    // implements to the set of unimplemented methods.
    unify_signature_maps(unify_super_interface_signatures(cls),
                         &class_signatures.unimplemented);

    // Mark all overriding methods as reachable via their parent method ref.
    for (auto* method : cls->get_vmethods()) {
      auto overridden_set = class_signatures.implemented.at(method->get_name())
                                .at(method->get_proto());
      for (auto overridden : overridden_set) {
        m_graph->add_edge(overridden, method);
      }
      // Replace the overridden methods by the overriding ones.
      update_signature_map(
          method, MethodSet{method}, &class_signatures.implemented);
    }

    // Mark all implementation methods as reachable via their interface methods.
    // Note that an interface method can be implemented by a method inherited
    // from a superclass.
    for (const auto& protos_pair : class_signatures.implemented) {
      for (const auto& ms_pair : protos_pair.second) {
        auto ms = ms_pair.second;
        always_assert(ms.size() == 1);
        auto implementation = *ms.begin();
        auto unimplemented_set =
            class_signatures.unimplemented.at(implementation->get_name())
                .at(implementation->get_proto());
        for (auto unimplemented : unimplemented_set) {
          m_graph->add_edge(unimplemented, implementation);
        }
        // Remove the method from the set of unimplemented interface methods.
        update_signature_map(
            implementation, MethodSet{}, &class_signatures.unimplemented);
      }
    }

    m_class_signature_maps.emplace(cls, class_signatures);
    return class_signatures;
  }

  SignatureMap analyze_interface(const DexClass* cls) {
    always_assert(is_interface(cls));
    if (m_interface_signature_maps.count(cls) != 0) {
      return m_interface_signature_maps.at(cls);
    }

    SignatureMap interface_signatures = unify_super_interface_signatures(cls);
    for (auto* method : cls->get_vmethods()) {
      auto overridden_set =
          interface_signatures.at(method->get_name()).at(method->get_proto());
      // These edges connect a method in a superinterface to the overriding
      // methods in a subinterface. A reference to the superinterface's method
      // will not resolve to the subinterface's method at runtime, but these
      // edges are critical because we do not add an edge between overridden
      // superinterface methods and their implementors. Concretely, given the
      // following code:
      //
      //   interface IA { void m(); }
      //   interface IB extends IA { void m(); }
      //   class C implements IB { void m(); }
      //
      // Our graph will contain an edge between IA::m and IB::m, and an edge
      // between IB::m and C::m. It will *not* contain an edge between IA::m and
      // C::m, even though C::m does implement IA::m as well. Therefore to get
      // all the implementors of IA::m, we need to traverse the edges added here
      // to find them. This design reduces the number of edges necessary for
      // building the graph.
      for (auto overridden : overridden_set) {
        m_graph->add_edge(overridden, method);
      }
      update_signature_map(method, MethodSet{method}, &interface_signatures);
    }

    m_interface_signature_maps.emplace(cls, interface_signatures);
    return interface_signatures;
  }

  SignatureMap unify_super_interface_signatures(const DexClass* cls) {
    SignatureMap super_interface_signatures;
    for (auto* intf : cls->get_interfaces()->get_type_list()) {
      auto intf_cls = type_class(intf);
      if (intf_cls != nullptr) {
        unify_signature_maps(analyze_interface(intf_cls),
                             &super_interface_signatures);
      }
    }
    return super_interface_signatures;
  }

  std::unique_ptr<Graph> m_graph;
  ClassSignatureMaps m_class_signature_maps;
  InterfaceSignatureMaps m_interface_signature_maps;
  const Scope& m_scope;
};

} // namespace

namespace method_override_graph {

Node Graph::empty_node;

const Node& Graph::get_node(const DexMethod* method) const {
  auto it = m_nodes.find(method);
  if (it == m_nodes.end()) {
    return empty_node;
  }
  return it->second;
}

void Graph::add_edge(const DexMethod* overridden, const DexMethod* overriding) {
  m_nodes.update(overridden,
                 [&](const DexMethod*, Node& node, bool /* exists */) {
                   node.children.insert(overriding);
                 });
  m_nodes.update(overriding,
                 [&](const DexMethod*, Node& node, bool /* exists */) {
                   node.parents.insert(overridden);
                 });
}

void Graph::dump(std::ostream& os) const {
  namespace bs = binary_serialization;
  bs::write_header(os, /* version */ 1);
  bs::GraphWriter<const DexMethod*> gw(
      [&](std::ostream& os, const DexMethod* method) {
        const auto& s = show_deobfuscated(method);
        bs::write<uint32_t>(os, s.size());
        os << s;
      },
      [&](const DexMethod* method) -> std::vector<const DexMethod*> {
        const auto& node = get_node(method);
        std::vector<const DexMethod*> succs(node.children.begin(),
                                            node.children.end());
        return succs;
      });
  gw.write(os, boost::adaptors::keys(m_nodes));
}

std::unique_ptr<const Graph> build_graph(const Scope& scope) {
  Timer t("Building method override graph");
  return GraphBuilder(scope).run();
}

std::unordered_set<const DexMethod*> get_overriding_methods(
    const Graph& graph, const DexMethod* method, bool include_interfaces) {
  std::unordered_set<const DexMethod*> overrides;
  std::unordered_set<const DexMethod*> visited;
  std::function<void(const DexMethod*, const Node&)> visit =
      [&](const DexMethod* current, const Node& node) {
        if (visited.count(current)) {
          return;
        }
        visited.emplace(current);
        for (const auto* child : node.children) {
          auto child_cls = type_class(child->get_class());
          if (include_interfaces || !is_interface(child_cls)) {
            overrides.emplace(child);
          }
          visit(child, graph.get_node(child));
        }
      };
  visit(method, graph.get_node(method));
  return overrides;
}

std::unordered_set<const DexMethod*> get_overridden_methods(
    const Graph& graph, const DexMethod* method, bool include_interfaces) {
  std::unordered_set<const DexMethod*> overridden;
  std::unordered_set<const DexMethod*> visited;
  std::function<void(const DexMethod*, const Node&)> visit =
      [&](const DexMethod* current, const Node& node) {
        if (visited.count(current)) {
          return;
        }
        visited.emplace(current);
        for (const auto* parent : node.parents) {
          auto parent_cls = type_class(parent->get_class());
          if (include_interfaces || !is_interface(parent_cls)) {
            overridden.emplace(parent);
          }
          visit(parent, graph.get_node(parent));
        }
      };
  visit(method, graph.get_node(method));
  return overridden;
}

bool is_true_virtual(const Graph& graph, const DexMethod* method) {
  if (is_abstract(method)) {
    return true;
  }
  const auto& node = graph.get_node(method);
  return node.parents.size() != 0 || node.children.size() != 0;
}

std::unordered_set<DexMethod*> get_non_true_virtuals(const Graph& graph,
                                                     const Scope& scope) {
  std::unordered_set<DexMethod*> non_true_virtuals;
  for (const auto* cls : scope) {
    for (auto* method : cls->get_vmethods()) {
      if (!is_true_virtual(graph, method)) {
        non_true_virtuals.emplace(method);
      }
    }
  }
  return non_true_virtuals;
}

} // namespace method_override_graph
