/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/Debug.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/StackInfo.h>
#include <AK/TemporaryChange.h>
#include <LibCore/ElapsedTimer.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Heap/HeapBlock.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/WeakContainer.h>
#include <LibJS/SafeFunction.h>
#include <setjmp.h>

#ifdef AK_OS_SERENITY
#    include <serenity.h>
#endif

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace JS {

#ifdef AK_OS_SERENITY
static int gc_perf_string_id;
#endif

// NOTE: We keep a per-thread list of custom ranges. This hinges on the assumption that there is one JS VM per thread.
static __thread HashMap<FlatPtr*, size_t>* s_custom_ranges_for_conservative_scan = nullptr;
static __thread HashMap<FlatPtr*, SourceLocation*>* s_safe_function_locations = nullptr;

Heap::Heap(VM& vm)
    : HeapBase(vm)
{
#ifdef AK_OS_SERENITY
    auto gc_signpost_string = "Garbage collection"sv;
    gc_perf_string_id = perf_register_string(gc_signpost_string.characters_without_null_termination(), gc_signpost_string.length());
#endif

    if constexpr (HeapBlock::min_possible_cell_size <= 16) {
        m_allocators.append(make<CellAllocator>(16));
    }
    static_assert(HeapBlock::min_possible_cell_size <= 24, "Heap Cell tracking uses too much data!");
    m_allocators.append(make<CellAllocator>(32));
    m_allocators.append(make<CellAllocator>(64));
    m_allocators.append(make<CellAllocator>(96));
    m_allocators.append(make<CellAllocator>(128));
    m_allocators.append(make<CellAllocator>(256));
    m_allocators.append(make<CellAllocator>(512));
    m_allocators.append(make<CellAllocator>(1024));
    m_allocators.append(make<CellAllocator>(3072));
}

Heap::~Heap()
{
    vm().string_cache().clear();
    vm().deprecated_string_cache().clear();
    collect_garbage(CollectionType::CollectEverything);
}

ALWAYS_INLINE CellAllocator& Heap::allocator_for_size(size_t cell_size)
{
    for (auto& allocator : m_allocators) {
        if (allocator->cell_size() >= cell_size)
            return *allocator;
    }
    dbgln("Cannot get CellAllocator for cell size {}, largest available is {}!", cell_size, m_allocators.last()->cell_size());
    VERIFY_NOT_REACHED();
}

Cell* Heap::allocate_cell(size_t size)
{
    if (should_collect_on_every_allocation()) {
        m_allocated_bytes_since_last_gc = 0;
        collect_garbage();
    } else if (m_allocated_bytes_since_last_gc + size > m_gc_bytes_threshold) {
        m_allocated_bytes_since_last_gc = 0;
        collect_garbage();
    }

    m_allocated_bytes_since_last_gc += size;
    auto& allocator = allocator_for_size(size);
    return allocator.allocate_cell(*this);
}

class GraphConstructorVisitor final : public Cell::Visitor {
public:
    explicit GraphConstructorVisitor(HashMap<Cell*, HeapRootTypeOrLocation> const& roots)
    {
        for (auto* root : roots.keys()) {
            visit(root);
            auto& graph_node = m_graph.ensure(reinterpret_cast<FlatPtr>(root));
            graph_node.class_name = root->class_name();
            graph_node.root_origin = *roots.get(root);
        }
    }

    virtual void visit_impl(Cell& cell) override
    {
        if (m_node_being_visited)
            m_node_being_visited->edges.set(reinterpret_cast<FlatPtr>(&cell));

        if (m_graph.get(reinterpret_cast<FlatPtr>(&cell)).has_value())
            return;

        m_work_queue.append(cell);
    }

    void visit_all_cells()
    {
        while (!m_work_queue.is_empty()) {
            auto ptr = reinterpret_cast<FlatPtr>(&m_work_queue.last());
            m_node_being_visited = &m_graph.ensure(ptr);
            m_node_being_visited->class_name = m_work_queue.last().class_name();
            m_work_queue.take_last().visit_edges(*this);
            m_node_being_visited = nullptr;
        }
    }

    void dump()
    {
        auto graph = AK::JsonObject();
        for (auto& it : m_graph) {
            AK::JsonArray edges;
            for (auto const& value : it.value.edges) {
                edges.must_append(DeprecatedString::formatted("{}", value));
            }

            auto node = AK::JsonObject();
            if (it.value.root_origin.has_value()) {
                it.value.root_origin->visit(
                    [&](HeapRootType location) {
                        switch (location) {
                        case HeapRootType::Handle:
                            node.set("root"sv, "Handle"sv);
                            return;
                        case HeapRootType::MarkedVector:
                            node.set("root"sv, "MarkedVector");
                            return;
                        case HeapRootType::RegisterPointer:
                            node.set("root"sv, "RegisterPointer");
                            return;
                        case HeapRootType::StackPointer:
                            node.set("root"sv, "StackPointer");
                            return;
                        case HeapRootType::VM:
                            node.set("root"sv, "VM");
                            return;
                        default:
                            VERIFY_NOT_REACHED();
                        }
                    },
                    [&](SourceLocation* location) {
                        node.set("root", DeprecatedString::formatted("SafeFunction {} {}:{}", location->function_name(), location->filename(), location->line_number()));
                    });
            }
            node.set("class_name"sv, it.value.class_name);
            node.set("edges"sv, edges);
            graph.set(DeprecatedString::number(it.key), node);
        }

        dbgln("{}", graph.to_deprecated_string());
    }

private:
    struct GraphNode {
        Optional<HeapRootTypeOrLocation> root_origin;
        StringView class_name;
        HashTable<FlatPtr> edges {};
    };

    GraphNode* m_node_being_visited { nullptr };
    Vector<Cell&> m_work_queue;
    HashMap<FlatPtr, GraphNode> m_graph;
};

void Heap::dump_graph()
{
    HashMap<Cell*, HeapRootTypeOrLocation> roots;
    gather_roots(roots);
    GraphConstructorVisitor visitor(roots);
    vm().bytecode_interpreter().visit_edges(visitor);
    visitor.visit_all_cells();
    visitor.dump();
}

void Heap::collect_garbage(CollectionType collection_type, bool print_report)
{
    VERIFY(!m_collecting_garbage);
    TemporaryChange change(m_collecting_garbage, true);

#ifdef AK_OS_SERENITY
    static size_t global_gc_counter = 0;
    perf_event(PERF_EVENT_SIGNPOST, gc_perf_string_id, global_gc_counter++);
#endif

    Core::ElapsedTimer collection_measurement_timer;
    if (print_report)
        collection_measurement_timer.start();

    if (collection_type == CollectionType::CollectGarbage) {
        if (m_gc_deferrals) {
            m_should_gc_when_deferral_ends = true;
            return;
        }
        HashMap<Cell*, HeapRootTypeOrLocation> roots;
        gather_roots(roots);
        mark_live_cells(roots);
    }
    finalize_unmarked_cells();
    sweep_dead_cells(print_report, collection_measurement_timer);
}

void Heap::gather_roots(HashMap<Cell*, HeapRootTypeOrLocation>& roots)
{
    vm().gather_roots(roots);
    gather_conservative_roots(roots);

    for (auto& handle : m_handles)
        roots.set(handle.cell(), HeapRootType::Handle);

    for (auto& vector : m_marked_vectors)
        vector.gather_roots(roots);

    if constexpr (HEAP_DEBUG) {
        dbgln("gather_roots:");
        for (auto* root : roots.keys())
            dbgln("  + {}", root);
    }
}

static void add_possible_value(HashMap<FlatPtr, HeapRootTypeOrLocation>& possible_pointers, FlatPtr data, HeapRootTypeOrLocation origin)
{
    if constexpr (sizeof(FlatPtr*) == sizeof(Value)) {
        // Because Value stores pointers in non-canonical form we have to check if the top bytes
        // match any pointer-backed tag, in that case we have to extract the pointer to its
        // canonical form and add that as a possible pointer.
        if ((data & SHIFTED_IS_CELL_PATTERN) == SHIFTED_IS_CELL_PATTERN)
            possible_pointers.set(Value::extract_pointer_bits(data), move(origin));
        else
            possible_pointers.set(data, move(origin));
    } else {
        static_assert((sizeof(Value) % sizeof(FlatPtr*)) == 0);
        // In the 32-bit case we will look at the top and bottom part of Value separately we just
        // add both the upper and lower bytes as possible pointers.
        possible_pointers.set(data, move(origin));
    }
}

#ifdef HAS_ADDRESS_SANITIZER
__attribute__((no_sanitize("address"))) void Heap::gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRootTypeOrLocation>& possible_pointers, FlatPtr addr)
{
    void* begin = nullptr;
    void* end = nullptr;
    void* real_stack = __asan_addr_is_in_fake_stack(__asan_get_current_fake_stack(), reinterpret_cast<void*>(addr), &begin, &end);

    if (real_stack != nullptr) {
        for (auto* real_stack_addr = reinterpret_cast<void const* const*>(begin); real_stack_addr < end; ++real_stack_addr) {
            void const* real_address = *real_stack_addr;
            if (real_address == nullptr)
                continue;
            add_possible_value(possible_pointers, reinterpret_cast<FlatPtr>(real_address), HeapRootType::StackPointer);
        }
    }
}
#else
void Heap::gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRootTypeOrLocation>&, FlatPtr)
{
}
#endif

__attribute__((no_sanitize("address"))) void Heap::gather_conservative_roots(HashMap<Cell*, HeapRootTypeOrLocation>& roots)
{
    FlatPtr dummy;

    dbgln_if(HEAP_DEBUG, "gather_conservative_roots:");

    jmp_buf buf;
    setjmp(buf);

    HashMap<FlatPtr, HeapRootTypeOrLocation> possible_pointers;

    auto* raw_jmp_buf = reinterpret_cast<FlatPtr const*>(buf);

    for (size_t i = 0; i < ((size_t)sizeof(buf)) / sizeof(FlatPtr); ++i)
        add_possible_value(possible_pointers, raw_jmp_buf[i], HeapRootType::RegisterPointer);

    auto stack_reference = bit_cast<FlatPtr>(&dummy);
    auto& stack_info = m_vm.stack_info();

    for (FlatPtr stack_address = stack_reference; stack_address < stack_info.top(); stack_address += sizeof(FlatPtr)) {
        auto data = *reinterpret_cast<FlatPtr*>(stack_address);
        add_possible_value(possible_pointers, data, HeapRootType::StackPointer);
        gather_asan_fake_stack_roots(possible_pointers, data);
    }

    // NOTE: If we have any custom ranges registered, scan those as well.
    //       This is where JS::SafeFunction closures get marked.
    if (s_custom_ranges_for_conservative_scan) {
        for (auto& custom_range : *s_custom_ranges_for_conservative_scan) {
            for (size_t i = 0; i < (custom_range.value / sizeof(FlatPtr)); ++i) {
                auto safe_function_location = s_safe_function_locations->get(custom_range.key);
                add_possible_value(possible_pointers, custom_range.key[i], *safe_function_location);
            }
        }
    }

    HashTable<HeapBlock*> all_live_heap_blocks;
    for_each_block([&](auto& block) {
        all_live_heap_blocks.set(&block);
        return IterationDecision::Continue;
    });

    for (auto possible_pointer : possible_pointers.keys()) {
        if (!possible_pointer)
            continue;
        dbgln_if(HEAP_DEBUG, "  ? {}", (void const*)possible_pointer);
        auto* possible_heap_block = HeapBlock::from_cell(reinterpret_cast<Cell const*>(possible_pointer));
        if (all_live_heap_blocks.contains(possible_heap_block)) {
            if (auto* cell = possible_heap_block->cell_from_possible_pointer(possible_pointer)) {
                if (cell->state() == Cell::State::Live) {
                    dbgln_if(HEAP_DEBUG, "  ?-> {}", (void const*)cell);
                    roots.set(cell, *possible_pointers.get(possible_pointer));
                } else {
                    dbgln_if(HEAP_DEBUG, "  #-> {}", (void const*)cell);
                }
            }
        }
    }
}

class MarkingVisitor final : public Cell::Visitor {
public:
    explicit MarkingVisitor(HashMap<Cell*, HeapRootTypeOrLocation> const& roots)
    {
        for (auto* root : roots.keys()) {
            visit(root);
        }
    }

    virtual void visit_impl(Cell& cell) override
    {
        if (cell.is_marked())
            return;
        dbgln_if(HEAP_DEBUG, "  ! {}", &cell);

        cell.set_marked(true);
        m_work_queue.append(cell);
    }

    void mark_all_live_cells()
    {
        while (!m_work_queue.is_empty()) {
            m_work_queue.take_last().visit_edges(*this);
        }
    }

private:
    Vector<Cell&> m_work_queue;
};

void Heap::mark_live_cells(HashMap<Cell*, HeapRootTypeOrLocation> const& roots)
{
    dbgln_if(HEAP_DEBUG, "mark_live_cells:");

    MarkingVisitor visitor(roots);

    vm().bytecode_interpreter().visit_edges(visitor);

    visitor.mark_all_live_cells();

    for (auto& inverse_root : m_uprooted_cells)
        inverse_root->set_marked(false);

    m_uprooted_cells.clear();
}

bool Heap::cell_must_survive_garbage_collection(Cell const& cell)
{
    if (!cell.overrides_must_survive_garbage_collection({}))
        return false;
    return cell.must_survive_garbage_collection();
}

void Heap::finalize_unmarked_cells()
{
    for_each_block([&](auto& block) {
        block.template for_each_cell_in_state<Cell::State::Live>([](Cell* cell) {
            if (!cell->is_marked() && !cell_must_survive_garbage_collection(*cell))
                cell->finalize();
        });
        return IterationDecision::Continue;
    });
}

void Heap::sweep_dead_cells(bool print_report, Core::ElapsedTimer const& measurement_timer)
{
    dbgln_if(HEAP_DEBUG, "sweep_dead_cells:");
    Vector<HeapBlock*, 32> empty_blocks;
    Vector<HeapBlock*, 32> full_blocks_that_became_usable;

    size_t collected_cells = 0;
    size_t live_cells = 0;
    size_t collected_cell_bytes = 0;
    size_t live_cell_bytes = 0;

    for_each_block([&](auto& block) {
        bool block_has_live_cells = false;
        bool block_was_full = block.is_full();
        block.template for_each_cell_in_state<Cell::State::Live>([&](Cell* cell) {
            if (!cell->is_marked() && !cell_must_survive_garbage_collection(*cell)) {
                dbgln_if(HEAP_DEBUG, "  ~ {}", cell);
                block.deallocate(cell);
                ++collected_cells;
                collected_cell_bytes += block.cell_size();
            } else {
                cell->set_marked(false);
                block_has_live_cells = true;
                ++live_cells;
                live_cell_bytes += block.cell_size();
            }
        });
        if (!block_has_live_cells)
            empty_blocks.append(&block);
        else if (block_was_full != block.is_full())
            full_blocks_that_became_usable.append(&block);
        return IterationDecision::Continue;
    });

    for (auto& weak_container : m_weak_containers)
        weak_container.remove_dead_cells({});

    for (auto* block : empty_blocks) {
        dbgln_if(HEAP_DEBUG, " - HeapBlock empty @ {}: cell_size={}", block, block->cell_size());
        allocator_for_size(block->cell_size()).block_did_become_empty({}, *block);
    }

    for (auto* block : full_blocks_that_became_usable) {
        dbgln_if(HEAP_DEBUG, " - HeapBlock usable again @ {}: cell_size={}", block, block->cell_size());
        allocator_for_size(block->cell_size()).block_did_become_usable({}, *block);
    }

    if constexpr (HEAP_DEBUG) {
        for_each_block([&](auto& block) {
            dbgln(" > Live HeapBlock @ {}: cell_size={}", &block, block.cell_size());
            return IterationDecision::Continue;
        });
    }

    m_gc_bytes_threshold = live_cell_bytes > GC_MIN_BYTES_THRESHOLD ? live_cell_bytes : GC_MIN_BYTES_THRESHOLD;

    if (print_report) {
        Duration const time_spent = measurement_timer.elapsed_time();
        size_t live_block_count = 0;
        for_each_block([&](auto&) {
            ++live_block_count;
            return IterationDecision::Continue;
        });

        dbgln("Garbage collection report");
        dbgln("=============================================");
        dbgln("     Time spent: {} ms", time_spent.to_milliseconds());
        dbgln("     Live cells: {} ({} bytes)", live_cells, live_cell_bytes);
        dbgln("Collected cells: {} ({} bytes)", collected_cells, collected_cell_bytes);
        dbgln("    Live blocks: {} ({} bytes)", live_block_count, live_block_count * HeapBlock::block_size);
        dbgln("   Freed blocks: {} ({} bytes)", empty_blocks.size(), empty_blocks.size() * HeapBlock::block_size);
        dbgln("=============================================");
    }
}

void Heap::did_create_handle(Badge<HandleImpl>, HandleImpl& impl)
{
    VERIFY(!m_handles.contains(impl));
    m_handles.append(impl);
}

void Heap::did_destroy_handle(Badge<HandleImpl>, HandleImpl& impl)
{
    VERIFY(m_handles.contains(impl));
    m_handles.remove(impl);
}

void Heap::did_create_marked_vector(Badge<MarkedVectorBase>, MarkedVectorBase& vector)
{
    VERIFY(!m_marked_vectors.contains(vector));
    m_marked_vectors.append(vector);
}

void Heap::did_destroy_marked_vector(Badge<MarkedVectorBase>, MarkedVectorBase& vector)
{
    VERIFY(m_marked_vectors.contains(vector));
    m_marked_vectors.remove(vector);
}

void Heap::did_create_weak_container(Badge<WeakContainer>, WeakContainer& set)
{
    VERIFY(!m_weak_containers.contains(set));
    m_weak_containers.append(set);
}

void Heap::did_destroy_weak_container(Badge<WeakContainer>, WeakContainer& set)
{
    VERIFY(m_weak_containers.contains(set));
    m_weak_containers.remove(set);
}

void Heap::defer_gc(Badge<DeferGC>)
{
    ++m_gc_deferrals;
}

void Heap::undefer_gc(Badge<DeferGC>)
{
    VERIFY(m_gc_deferrals > 0);
    --m_gc_deferrals;

    if (!m_gc_deferrals) {
        if (m_should_gc_when_deferral_ends)
            collect_garbage();
        m_should_gc_when_deferral_ends = false;
    }
}

void Heap::uproot_cell(Cell* cell)
{
    m_uprooted_cells.append(cell);
}

void register_safe_function_closure(void* base, size_t size, SourceLocation* location)
{
    if (!s_custom_ranges_for_conservative_scan) {
        // FIXME: This per-thread HashMap is currently leaked on thread exit.
        s_custom_ranges_for_conservative_scan = new HashMap<FlatPtr*, size_t>;
    }
    if (!s_safe_function_locations) {
        s_safe_function_locations = new HashMap<FlatPtr*, SourceLocation*>;
    }
    auto result = s_custom_ranges_for_conservative_scan->set(reinterpret_cast<FlatPtr*>(base), size);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    result = s_safe_function_locations->set(reinterpret_cast<FlatPtr*>(base), location);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void unregister_safe_function_closure(void* base, size_t, SourceLocation*)
{
    VERIFY(s_custom_ranges_for_conservative_scan);
    bool did_remove_range = s_custom_ranges_for_conservative_scan->remove(reinterpret_cast<FlatPtr*>(base));
    VERIFY(did_remove_range);
    bool did_remove_location = s_safe_function_locations->remove(reinterpret_cast<FlatPtr*>(base));
    VERIFY(did_remove_location);
}

}
