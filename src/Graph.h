/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4C_GRAPH_H
#define VC4C_GRAPH_H

#include "CompilationError.h"
#include "performance.h"

#include <algorithm>
#include <functional>
#include <type_traits>

namespace vc4c
{
    struct empty_base
    {
    };

    /*
     * The possible directionality of the graph, whether edges can be directed or not
     */
    enum class Directionality
    {
        UNDIRECTED,
        DIRECTED,
        BIDIRECTIONAL
    };

    template <typename Key, typename Relation, Directionality Direction, typename Base = empty_base>
    class Node;

    template <typename NodeType, typename Relation, Directionality Direction>
    class Edge;

    template <typename Key, typename NodeType>
    class Graph;

    /*
     * A node in a graph, general base-class maintaining the list of edges to neighboring nodes
     */
    template <typename Key, typename Relation, Directionality Direction, typename Base>
    class Node : public Base
    {
    public:
        using RelationType = Relation;
        using NodeType = Node<Key, Relation, Direction, Base>;
        using EdgeType = Edge<NodeType, Relation, Direction>;
        using GraphType = Graph<Key, NodeType>;

        template <typename... Args>
        explicit Node(GraphType& graph, const Key& key, Args&&... args) :
            Base(std::forward<Args&&>(args)...), key(key), graph(graph), edges()
        {
        }
        template <typename... Args>
        explicit Node(GraphType& graph, Key&& key, Args&&... args) :
            Base(std::forward<Args&&>(args)...), key(key), graph(graph), edges()
        {
        }

        Node(const Node&) = delete;
        Node(Node&&) noexcept = delete;
        ~Node() noexcept = default;

        Node& operator=(const Node&) = delete;
        Node& operator=(Node&&) noexcept = delete;

        void erase()
        {
            graph.eraseNode(key);
        }

        /*!
         * Adds the given neighbor with the given relation.
         * Multiple calls to this method do not override the previous association.
         */
        EdgeType* addEdge(Node* neighbor, Relation&& relation)
        {
            if(isAdjacent(neighbor))
                throw CompilationError(CompilationStep::GENERAL, "Nodes are already adjacent!");
            return graph.createEdge(this, neighbor, std::forward<Relation&&>(relation));
        }

        EdgeType& getOrCreateEdge(Node* neighbor, Relation&& defaultRelation = {})
        {
            auto it = edges.find(neighbor);
            if(it != edges.end())
                return *it->second;
            return *addEdge(neighbor, std::forward<Relation&&>(defaultRelation));
        }

        EdgeType* findEdge(const Relation& relation)
        {
            for(auto& pair : edges)
            {
                if(pair.second->data == relation)
                    return pair.second;
            }
            return nullptr;
        }

        const EdgeType* findEdge(const Relation& relation) const
        {
            for(const auto& pair : edges)
            {
                if(pair.second->data == relation)
                    return pair.second;
            }
            return nullptr;
        }

        EdgeType* findEdge(const std::function<bool(const Relation&)>& predicate)
        {
            for(auto& pair : edges)
            {
                if(predicate(pair.second->data))
                    return pair.second;
            }
            return nullptr;
        }

        const EdgeType* findEdge(const std::function<bool(const Relation&)>& predicate) const
        {
            for(const auto& pair : edges)
            {
                if(predicate(pair.second->data))
                    return pair.second;
            }
            return nullptr;
        }

        void removeEdge(EdgeType& edge)
        {
            graph.eraseEdge(edge);
        }

        void removeAsNeighbor(Node* neighbor)
        {
            auto it = edges.find(neighbor);
            if(it == edges.end())
                throw CompilationError(CompilationStep::GENERAL, "Node was not neighbor of this node!");
            removeEdge(*it->second);
        }

        /*
         * Returns the single neighbor with the given relation.
         * Returns nullptr otherwise, if there is no or more than one neighbor with this relation.
         */
        inline const Node* getSingleNeighbor(const Relation relation) const
        {
            return getSingleNeighbor([&relation](const Relation& rel) -> bool { return rel == relation; });
        }

        inline Node* getSingleNeighbor(const Relation relation)
        {
            return getSingleNeighbor([&relation](const Relation& rel) -> bool { return rel == relation; });
        }

        /*
         * Returns the single neighbor where the relation matches the given predicate.
         * Returns nullptr otherwise, if there is no or more than one neighbor with this relation.
         */
        const Node* getSingleNeighbor(const std::function<bool(const Relation&)>& relation) const
        {
            static_assert(Direction == Directionality::UNDIRECTED,
                "For directed graphs, incoming and outgoing edges need to be handled differently!");
            Node* singleNeighbor = nullptr;
            for(const auto& pair : edges)
            {
                if(relation(pair.second->data))
                {
                    if(singleNeighbor != nullptr)
                        // multiple neighbors
                        return nullptr;
                    singleNeighbor = pair.first;
                }
            }
            return singleNeighbor;
        }

        Node* getSingleNeighbor(const std::function<bool(const Relation&)>& relation)
        {
            static_assert(Direction == Directionality::UNDIRECTED,
                "For directed graphs, incoming and outgoing edges need to be handled differently!");
            Node* singleNeighbor = nullptr;
            for(auto& pair : edges)
            {
                if(relation(pair.second->data))
                {
                    if(singleNeighbor != nullptr)
                        // multiple neighbors
                        return nullptr;
                    singleNeighbor = pair.first;
                }
            }
            return singleNeighbor;
        }

        Node* getSinglePredecessor(const std::function<bool(const Relation&)>& relation)
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have predecessors!");
            Node* singlePredecessor = nullptr;
            for(auto& edge : edges)
            {
                if(edge.second->isOutput(*this) && relation(edge.second->data))
                {
                    if(singlePredecessor != nullptr)
                        // multiple predecessors
                        return nullptr;
                    singlePredecessor = edge.first;
                }
            }
            return singlePredecessor;
        }

        Node* getSinglePredecessor()
        {
            return getSinglePredecessor([](const Relation& rel) -> bool { return true; });
        }

        const Node* getSinglePredecessor(const std::function<bool(const Relation&)>& relation) const
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have predecessors!");
            const Node* singlePredecessor = nullptr;
            for(auto& edge : edges)
            {
                if(edge.second->isOutput(*this) && relation(edge.second->data))
                {
                    if(singlePredecessor != nullptr)
                        // multiple predecessors
                        return nullptr;
                    singlePredecessor = edge.first;
                }
            }
            return singlePredecessor;
        }

        const Node* getSinglePredecessor() const
        {
            return getSinglePredecessor([](const Relation& rel) -> bool { return true; });
        }

        Node* getSingleSuccessor(const std::function<bool(const Relation&)>& relation)
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have successors!");
            Node* singleSuccessor = nullptr;
            for(auto& edge : edges)
            {
                if(edge.second->isInput(*this) && relation(edge.second->data))
                {
                    if(singleSuccessor != nullptr)
                        // multiple successors
                        return nullptr;
                    singleSuccessor = edge.first;
                }
            }
            return singleSuccessor;
        }

        Node* getSingleSuccessor()
        {
            return getSingleSuccessor([](const Relation& rel) -> bool { return true; });
        }

        const Node* getSingleSuccessor(const std::function<bool(const Relation&)>& relation) const
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have successors!");
            const Node* singleSuccessor = nullptr;
            for(auto& edge : edges)
            {
                if(edge.second->isInput(*this) && relation(edge.second->data))
                {
                    if(singleSuccessor != nullptr)
                        // multiple successors
                        return nullptr;
                    singleSuccessor = edge.first;
                }
            }
            return singleSuccessor;
        }

        const Node* getSingleSuccessor() const
        {
            return getSingleSuccessor([](const Relation& rel) -> bool { return true; });
        }

        bool isAdjacent(NodeType* node) const
        {
            return getEdge(node);
        }

        inline bool isAdjacent(const NodeType* node) const
        {
            return getEdge(node);
        }

        inline EdgeType* getEdge(NodeType* node) const
        {
            auto it = edges.find(node);
            return it != edges.end() ? it->second : nullptr;
        }

        inline EdgeType* getEdge(const NodeType* node) const
        {
            auto it = edges.find(const_cast<NodeType*>(node));
            return it != edges.end() ? it->second : nullptr;
        }

        /*
         * Executes the given predicate for all neighbors until it becomes false
         */
        void forAllEdges(const std::function<bool(NodeType&, EdgeType&)>& predicate)
        {
            static_assert(Direction == Directionality::UNDIRECTED,
                "For directed graphs, incoming and outgoing edges need to be handled differently!");
            for(auto& pair : edges)
            {
                if(!predicate(*pair.first, *pair.second))
                    return;
            }
        }

        void forAllEdges(const std::function<bool(const NodeType&, const EdgeType&)>& predicate) const
        {
            static_assert(Direction == Directionality::UNDIRECTED,
                "For directed graphs, incoming and outgoing edges need to be handled differently!");
            for(const auto& pair : edges)
            {
                if(!predicate(*pair.first, *pair.second))
                    return;
            }
        }

        /*
         * Executes the predicate for all incoming edges, until it becomes false
         */
        void forAllIncomingEdges(const std::function<bool(NodeType&, EdgeType&)>& predicate)
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have incoming edges!");
            for(auto& edge : edges)
            {
                if(edge.second->isOutput(*this))
                {
                    if(!predicate(*edge.first, *edge.second))
                        return;
                }
            }
        }

        void forAllIncomingEdges(const std::function<bool(const NodeType&, const EdgeType&)>& predicate) const
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have incoming edges!");
            for(const auto& edge : edges)
            {
                if(edge.second->isOutput(*this))
                {
                    if(!predicate(*edge.first, *edge.second))
                        return;
                }
            }
        }

        /*
         * Executes the predicate for all outgoing edges, until it becomes false
         */
        void forAllOutgoingEdges(const std::function<bool(NodeType&, EdgeType&)>& predicate)
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have outgoing edges!");
            for(auto& edge : edges)
            {
                if(edge.second->isInput(*this))
                {
                    if(!predicate(*edge.first, *edge.second))
                        return;
                }
            }
        }

        void forAllOutgoingEdges(const std::function<bool(const NodeType&, const EdgeType&)>& predicate) const
        {
            static_assert(Direction != Directionality::UNDIRECTED, "Only directed graphs have outgoing edges!");
            for(const auto& edge : edges)
            {
                if(edge.second->isInput(*this))
                {
                    if(!predicate(*edge.first, *edge.second))
                        return;
                }
            }
        }

        std::size_t getEdgesSize() const
        {
            return edges.size();
        }

        // NOTE: Since the reserve forces a rehashing, this should be called for nodes without edges only!
        void reserveEdgesSize(std::size_t numEdges)
        {
            edges.reserve(numEdges);
        }

        bool isSource() const
        {
            static_assert(
                EdgeType::Directed != Directionality::UNDIRECTED, "Can only find sources in directed graphs!");
            bool hasIncomingEdges = false;
            forAllIncomingEdges([&](const NodeType&, const EdgeType&) -> bool {
                hasIncomingEdges = true;
                return false;
            });
            return !hasIncomingEdges;
        }

        bool isSink() const
        {
            static_assert(EdgeType::Directed != Directionality::UNDIRECTED, "Can only find sinks in directed graphs!");
            bool hasOutgoingEdges = false;
            forAllOutgoingEdges([&](const NodeType&, const EdgeType&) -> bool {
                hasOutgoingEdges = true;
                return false;
            });
            return !hasOutgoingEdges;
        }

        Key key;

    protected:
        GraphType& graph;
        FastMap<NodeType*, EdgeType*> edges;

        friend GraphType;
    };

    /*
     * The actual direction an edge is pointing.
     */
    enum class Direction : unsigned char
    {
        NONE = 0,
        FIRST_TO_SECOND = 1,
        SECOND_TO_FIRST = 2,
        BOTH = 3
    };

    /**
     * Base for any undirected edge
     */
    template <typename Node>
    class UndirectedEdge
    {
    public:
        FastSet<Node*> getNodes()
        {
            return FastSet<Node*>{&first, &second};
        }

        enum Direction getDirection() const noexcept
        {
            return Direction::NONE;
        }

    protected:
        UndirectedEdge(Node& f, Node& s) : first(f), second(s) {}

        Node& first;
        Node& second;
    };

    /**
     * Base class for any (bi-)directional edge
     */
    template <typename Node>
    class DirectedEdge
    {
    public:
        FastSet<Node*> getNodes()
        {
            return FastSet<Node*>{&first, &second};
        }

        FastSet<const Node*> getNodes() const
        {
            return FastSet<const Node*>{&first, &second};
        }

        bool isInput(const Node& node) const
        {
            return (&node == &first && firstInput) || (&node == &second && secondInput);
        }

        bool isOutput(const Node& node) const
        {
            return (&node == &second && firstInput) || (&node == &first && secondInput);
        }

        enum Direction getDirection() const noexcept
        {
            if(firstInput && secondInput)
                return Direction::BOTH;
            if(firstInput)
                return Direction::FIRST_TO_SECOND;
            return Direction::SECOND_TO_FIRST;
        }

    protected:
        DirectedEdge(Node& f, Node& s) : first(f), second(s), firstInput(true), secondInput(false) {}

        Node& first;
        Node& second;
        bool firstInput;
        bool secondInput;
    };

    /**
     * Base class for any unidirectional edge
     */
    template <typename Node>
    class UnidirectionalEdge : public DirectedEdge<Node>
    {
        using Base = DirectedEdge<Node>;

    public:
        Node& getInput()
        {
            return this->first;
        }

        const Node& getInput() const
        {
            return this->first;
        }

        Node& getOutput()
        {
            return this->second;
        }

        const Node& getOutput() const
        {
            return this->second;
        }

        enum Direction getDirection() const noexcept
        {
            return Direction::FIRST_TO_SECOND;
        }

    protected:
        UnidirectionalEdge(Node& f, Node& s) : Base(f, s) {}
    };

    /*
     * An edge represents the connection between two nodes.
     *
     * Edges store additional content specifying the type of relation/connection between the nodes connected by the edge
     *
     * If the edge is directional, then the edge points from the first node to the second node
     *
     */
    template <typename Node, typename Relation, Directionality Direction>
    class Edge : public std::conditional<Direction == Directionality::UNDIRECTED, UndirectedEdge<Node>,
                     typename std::conditional<Direction == Directionality::DIRECTED, UnidirectionalEdge<Node>,
                         DirectedEdge<Node>>::type>::type
    {
        using Base = typename std::conditional<Direction == Directionality::UNDIRECTED, UndirectedEdge<Node>,
            typename std::conditional<Direction == Directionality::DIRECTED, UnidirectionalEdge<Node>,
                DirectedEdge<Node>>::type>::type;

    public:
        using NodeType = Node;

        Edge(NodeType& first, NodeType& second, Relation&& data) : Base(first, second), data(data) {}

        Edge(const Edge&) = delete;
        Edge(Edge&&) noexcept = delete;
        ~Edge() noexcept = default;

        Edge& operator=(const Edge&) = delete;
        Edge& operator=(Edge&&) noexcept = delete;

        bool operator==(const Edge& other) const
        {
            return &this->first == &other.first && &this->second == &other.second;
        }

        NodeType& getOtherNode(const NodeType& oneNode)
        {
            if(&this->first == &oneNode)
                return this->second;
            return this->first;
        }

        const NodeType& getOtherNode(const NodeType& oneNode) const
        {
            if(&this->first == &oneNode)
                return this->second;
            return this->first;
        }

        Edge& addInput(const NodeType& node)
        {
            static_assert(Direction == Directionality::BIDIRECTIONAL, "Can only add input for bidirectional graphs!");
            if(&node == &this->first)
                this->firstInput = true;
            else if(&node == &this->second)
                this->secondInput = true;
            else
                throw CompilationError(CompilationStep::GENERAL, "Node is not a part of this edge!");
            return *this;
        }

        static constexpr Directionality Directed = Direction;

        Relation data;

    protected:
        friend NodeType;
        friend typename NodeType::GraphType;
        friend struct std::hash<Edge<Node, Relation, Directed>>;
    };

    /*
     * General base type for graphs of any kind.
     *
     * A graph contains nodes containing the object being represented as well as some arbitrary additional information.
     * Additionally, the object-type of the relations between the nodes can be specified allowing for extra data being
     * stored in them.
     *
     * NOTE: The fact whether the graph is directed or not must be managed by the user.
     * E.g. for an undirected graph, a relationship must be added to both nodes taking place in it.
     */
    template <typename Key, typename NodeType>
    class Graph
    {
    public:
        using RelationType = typename NodeType::RelationType;
        using EdgeType = typename NodeType::EdgeType;

        explicit Graph(std::size_t numNodes = 0) : nodes(), edges()
        {
            reserveNodeSize(numNodes);
        }
        Graph(const Graph&) = delete;
        Graph(Graph&&) noexcept = delete;
        ~Graph() noexcept = default;

        Graph& operator=(const Graph&) = delete;
        Graph& operator=(Graph&&) noexcept = delete;

        /*
         * Returns the node for the given key
         *
         * If such a node does not exist yet, a new node is created with the given additional initial payload
         */
        template <typename... Args>
        NodeType& getOrCreateNode(Key key, Args&&... initialPayload)
        {
            auto it = nodes.find(key);
            if(it == nodes.end())
            {
                return nodes
                    .emplace(std::piecewise_construct_t{}, std::forward_as_tuple(key),
                        std::forward_as_tuple(*this, key, std::forward<Args&&>(initialPayload)...))
                    .first->second;
            }
            return it->second;
        }

        /*
         * Guarantees a node for the given key to exist within the graph and returns it.
         * Throws a compilation-error otherwise
         */
        NodeType& assertNode(const Key& key)
        {
            auto it = nodes.find(key);
            if(it == nodes.end())
            {
                throw CompilationError(CompilationStep::GENERAL, "Failed to find graph-node for key");
            }
            return it->second;
        }

        const NodeType& assertNode(const Key& key) const
        {
            auto it = nodes.find(key);
            if(it == nodes.end())
            {
                throw CompilationError(CompilationStep::GENERAL, "Failed to find graph-node for key");
            }
            return it->second;
        }

        NodeType* findNode(const Key& key)
        {
            auto it = nodes.find(key);
            if(it == nodes.end())
                return nullptr;
            return &it->second;
        }

        const NodeType* findNode(const Key& key) const
        {
            auto it = nodes.find(key);
            if(it == nodes.end())
                return nullptr;
            return &it->second;
        }

        void eraseNode(const Key& key)
        {
            auto it = nodes.find(key);
            if(it == nodes.end())
                throw CompilationError(CompilationStep::GENERAL, "Failed to find graph-node for key");
            for(auto& edge : it->second.edges)
            {
                edge.second->getOtherNode(it->second).edges.erase(&it->second);
                edges.erase(*edge.second);
            }
            nodes.erase(it);
        }

        /*
         * Finds a source in this graph (a node without incoming edges)
         */
        NodeType* findSource()
        {
            static_assert(
                EdgeType::Directed != Directionality::UNDIRECTED, "Can only find sources in directed graphs!");
            for(auto& pair : nodes)
            {
                if(pair.second.isSource())
                    return &pair.second;
            }
            return nullptr;
        }

        /*
         * Executes the consumer for all sources (nodes without incoming edges) of this graph until
         * a) there are no more sources or
         * b) the consumer returns false
         */
        void forAllSources(const std::function<bool(const NodeType& source)>& consumer)
        {
            static_assert(
                EdgeType::Directed != Directionality::UNDIRECTED, "Can only find sources in directed graphs!");
            for(auto& pair : nodes)
            {
                if(pair.second.isSource())
                {
                    if(!consumer(pair.second))
                        return;
                }
            }
        }

        /*
         * Finds a sink in this graph (a node without outgoing edges)
         */
        NodeType* findSink()
        {
            static_assert(EdgeType::Directed != Directionality::UNDIRECTED, "Can only find sinks in directed graphs!");
            for(auto& pair : nodes)
            {
                if(pair.second.isSink())
                    return &pair.second;
            }
            return nullptr;
        }

        /*
         * Executes the consumer for all sinks (nodes without outgoing edges) of this graph until
         * a) there are no more sinks or
         * b) the consumer returns false
         */
        void forAllSinks(const std::function<bool(const NodeType& source)>& consumer)
        {
            static_assert(EdgeType::Directed != Directionality::UNDIRECTED, "Can only find sinks in directed graphs!");
            for(auto& pair : nodes)
            {
                if(pair.second.isSink())
                {
                    if(!consumer(pair.second))
                        return;
                }
            }
        }

        const FastMap<Key, NodeType>& getNodes() const
        {
            return nodes;
        }

        FastMap<Key, NodeType>& getNodes()
        {
            return nodes;
        }

        void forAllNodes(const std::function<void(NodeType&)>& consumer)
        {
            std::for_each(nodes.begin(), nodes.end(), [&](auto& pair) { consumer(pair.second); });
        }

        void forAllNodes(const std::function<void(const NodeType&)>& consumer) const
        {
            std::for_each(nodes.begin(), nodes.end(), [&](const auto& pair) { consumer(pair.second); });
        }

        void clear()
        {
            nodes.clear();
            edges.clear();
        }

        // NOTE: Since the reserve forces a rehashing, this should be called for empty graphs only!
        void reserveNodeSize(std::size_t numNodes)
        {
            nodes.reserve(numNodes);
            // it is safe to assume we have at least 1 edge per node
            edges.reserve(numNodes);
        }

    protected:
        FastMap<Key, NodeType> nodes;
        FastSet<EdgeType> edges;

        EdgeType* createEdge(NodeType* first, NodeType* second, RelationType&& relation)
        {
            EdgeType& edge =
                const_cast<EdgeType&>(*(edges.emplace(*first, *second, std::forward<RelationType&&>(relation)).first));
            first->edges.emplace(second, &edge);
            second->edges.emplace(first, &edge);
            return &edge;
        }

        void eraseEdge(EdgeType& edge)
        {
            edge.first.edges.erase(&edge.second);
            edge.second.edges.erase(&edge.first);
            edges.erase(edge);
        }

        friend NodeType;
    };
} // namespace vc4c

namespace std
{
    template <typename Node, typename Relation, vc4c::Directionality Direction>
    struct hash<vc4c::Edge<Node, Relation, Direction>>
    {
        using EdgeType = vc4c::Edge<Node, Relation, Direction>;
        inline std::size_t operator()(const EdgeType& edge) const
        {
            std::hash<Node*> h;
            return h(&edge.first) ^ h(&edge.second);
        }
    };
} /* namespace std */

#endif /* VC4C_GRAPH_H */
