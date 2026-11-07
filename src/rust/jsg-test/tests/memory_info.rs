// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What Rust resources report to heap snapshots, through `jsg::MemoryTracker`.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::collections::HashMap;
use std::collections::HashSet;
use std::collections::VecDeque;
use std::ffi::CStr;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_resource;

// =============================================================================
// Snapshot parsing
// =============================================================================

/// A heap snapshot, parsed from V8's JSON format.
pub struct Snapshot {
    pub nodes: Vec<Node>,
}

pub struct Node {
    pub kind: String,
    pub name: String,
    pub self_size: u64,
    /// `(edge name, index of the node it points to)`.
    pub edges: Vec<(String, usize)>,
    /// The type of each edge, in the order of `edges`: `internal`, `element`, ...
    pub edge_types: Vec<String>,
}

impl Snapshot {
    pub fn take(lock: &mut jsg::Lock<'_>) -> Self {
        Self::parse(&crate::Harness::take_heap_snapshot(lock))
    }

    fn parse(json: &str) -> Self {
        let value: serde_json::Value = serde_json::from_str(json).unwrap();
        let meta = &value["snapshot"]["meta"];
        let field_index = |fields: &serde_json::Value, name: &str| {
            fields
                .as_array()
                .unwrap()
                .iter()
                .position(|field| field == name)
                .unwrap()
        };
        let node_fields = &meta["node_fields"];
        let edge_fields = &meta["edge_fields"];
        let node_field_count = node_fields.as_array().unwrap().len();
        let edge_field_count = edge_fields.as_array().unwrap().len();
        let node_type_field = field_index(node_fields, "type");
        let node_name_field = field_index(node_fields, "name");
        let node_size_field = field_index(node_fields, "self_size");
        let node_edge_count_field = field_index(node_fields, "edge_count");
        let edge_type_field = field_index(edge_fields, "type");
        let edge_name_field = field_index(edge_fields, "name_or_index");
        let edge_to_field = field_index(edge_fields, "to_node");
        let node_types = meta["node_types"][node_type_field].as_array().unwrap();
        let edge_types = meta["edge_types"][edge_type_field].as_array().unwrap();

        let strings: Vec<&str> = value["strings"]
            .as_array()
            .unwrap()
            .iter()
            .map(|s| s.as_str().unwrap())
            .collect();
        let numbers = |key: &str| -> Vec<u64> {
            value[key]
                .as_array()
                .unwrap()
                .iter()
                .map(|n| n.as_u64().unwrap())
                .collect()
        };
        let raw_nodes = numbers("nodes");
        let raw_edges = numbers("edges");
        let to_usize = |n: u64| usize::try_from(n).unwrap();

        let mut nodes = Vec::new();
        let mut edges = raw_edges.chunks(edge_field_count);
        for node in raw_nodes.chunks(node_field_count) {
            let edge_count = to_usize(node[node_edge_count_field]);
            let mut node_edge_types = Vec::new();
            let node_edges = edges
                .by_ref()
                .take(edge_count)
                .map(|edge| {
                    let edge_type = edge_types[to_usize(edge[edge_type_field])]
                        .as_str()
                        .unwrap();
                    node_edge_types.push(edge_type.to_owned());
                    let name_or_index = edge[edge_name_field];
                    let name = if edge_type == "element" || edge_type == "hidden" {
                        name_or_index.to_string()
                    } else {
                        strings[to_usize(name_or_index)].to_owned()
                    };
                    (name, to_usize(edge[edge_to_field]) / node_field_count)
                })
                .collect();
            nodes.push(Node {
                kind: node_types[to_usize(node[node_type_field])]
                    .as_str()
                    .unwrap()
                    .to_owned(),
                name: strings[to_usize(node[node_name_field])].to_owned(),
                self_size: node[node_size_field],
                edges: node_edges,
                edge_types: node_edge_types,
            });
        }
        Self { nodes }
    }

    /// The indices of the nodes that Rust or C++ reported as `name`.
    pub fn find_all(&self, name: &str) -> Vec<usize> {
        let name = format!("workerd / {name}");
        (0..self.nodes.len())
            .filter(|&i| self.nodes[i].name == name)
            .collect()
    }

    /// The index of the one node reported as `name`.
    pub fn find(&self, name: &str) -> usize {
        let found = self.find_all(name);
        assert_eq!(found.len(), 1, "expected one node named {name}");
        found[0]
    }

    /// The nodes that the edges named `edge` of node `from` point to.
    pub fn targets(&self, from: usize, edge: &str) -> Vec<usize> {
        self.nodes[from]
            .edges
            .iter()
            .filter(|(name, _)| name == edge)
            .map(|&(_, to)| to)
            .collect()
    }

    /// The node that the one edge named `edge` of node `from` points to.
    pub fn target(&self, from: usize, edge: &str) -> usize {
        let targets = self.targets(from, edge);
        assert_eq!(
            targets.len(),
            1,
            "expected one edge {edge} from {}",
            self.nodes[from].name
        );
        targets[0]
    }

    pub fn edge_names(&self, from: usize) -> Vec<&str> {
        self.nodes[from]
            .edges
            .iter()
            .map(|(name, _)| name.as_str())
            .collect()
    }

    /// The `(type, name)` of each edge of node `from`.
    pub fn edges(&self, from: usize) -> Vec<(&str, &str)> {
        let node = &self.nodes[from];
        node.edge_types
            .iter()
            .zip(&node.edges)
            .map(|(edge_type, (name, _))| (edge_type.as_str(), name.as_str()))
            .collect()
    }

    /// The names of the nodes that node `from` has edges to, in order.
    pub fn target_names(&self, from: usize) -> Vec<&str> {
        self.nodes[from]
            .edges
            .iter()
            .map(|&(_, to)| self.nodes[to].name.as_str())
            .collect()
    }

    fn name(&self, node: usize) -> &str {
        &self.nodes[node].name
    }

    fn size(&self, node: usize) -> usize {
        usize::try_from(self.nodes[node].self_size).unwrap()
    }
}

// =============================================================================
// Resources
// =============================================================================

#[jsg_resource(memory_info)]
struct Child {
    pub parent: jsg::RefCell<Option<jsg::Rc<Parent>>>,
}

#[jsg_resource]
impl Child {}

fn new_child(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Child> {
    jsg::Rc::new(
        lock,
        Child {
            parent: jsg::RefCell::new(None),
        },
    )
}

/// A helper that is a node of its own, like a C++ type with `JSG_MEMORY_INFO`.
struct Cache {
    entries: HashMap<String, String>,
}

impl jsg::MemoryRetainer for Cache {
    fn memory_name(&self) -> &'static CStr {
        c"Cache"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"entries", &self.entries);
    }
}

jsg::jsg_memory_retainer!(Cache);

jsg::jsg_traced!(Cache);

fn new_cache() -> Cache {
    Cache {
        entries: HashMap::from([("key".to_owned(), "value".to_owned())]),
    }
}

#[jsg_resource(memory_info)]
struct Parent {
    pub name: String,
    pub child: jsg::Rc<Child>,
    pub children: Vec<jsg::Rc<Child>>,
    pub numbers: Vec<u32>,
    pub cache: Cache,
    pub object: v8::TracedGlobal<v8::Value>,
    pub missing: Option<jsg::Rc<Child>>,
    #[jsg_memory_info(skip)]
    pub unreported: String,
}

#[jsg_resource]
impl Parent {}

/// A resource without `memory_info`, which reports no fields.
#[jsg_resource]
struct Silent {
    pub name: String,
}

#[jsg_resource]
impl Silent {}

fn parent(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) -> jsg::Rc<Parent> {
    let child = new_child(lock);
    parent_of(lock, ctx, child)
}

fn parent_of(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    child: jsg::Rc<Child>,
) -> jsg::Rc<Parent> {
    let object = ctx.eval_raw(lock, "({ marker: 1 })").unwrap();
    let parent = Parent {
        name: "a name".to_owned(),
        child,
        children: vec![new_child(lock), new_child(lock)],
        numbers: vec![1, 2, 3],
        cache: new_cache(),
        object: v8::TracedGlobal::new(lock, object),
        missing: None,
        unreported: "unreported".to_owned(),
    };
    jsg::Rc::new(lock, parent)
}

/// Wraps `resource` and stores the wrapper as the global `name`, so that the snapshot reaches it
/// through the isolate's list of wrapped objects.
fn expose<R: jsg::Resource>(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    name: &str,
    resource: &jsg::Rc<R>,
) -> Result<(), jsg::Error> {
    let wrapper = resource.clone().to_js(lock)?;
    ctx.set_global(lock, name, wrapper);
    Ok(())
}

// =============================================================================
// Resource nodes
// =============================================================================

#[test]
fn resource_node_reports_its_fields() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = parent(lock, ctx);
        expose(lock, ctx, "parent", &parent)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Parent");

        // The node is sized as the resource plus its Wrappable.
        assert_eq!(
            snapshot.size(node),
            size_of::<Parent>() + crate::Harness::rust_wrappable_size()
        );

        let name = snapshot.target(node, "name");
        assert_eq!(snapshot.name(name), "workerd / String");
        assert_eq!(snapshot.size(name), parent.name.capacity());

        let child = snapshot.target(node, "child");
        assert_eq!(snapshot.name(child), "workerd / Child");

        let children = snapshot.target(node, "children");
        assert_eq!(snapshot.name(children), "workerd / Vec");
        assert_eq!(
            snapshot.target_names(children),
            ["workerd / Child", "workerd / Child"]
        );

        // Plain data is one node of the allocation's size.
        let numbers = snapshot.target(node, "numbers");
        assert_eq!(snapshot.name(numbers), "workerd / Vec");
        assert!(snapshot.nodes[numbers].edges.is_empty());
        assert_eq!(
            snapshot.size(numbers),
            parent.numbers.capacity() * size_of::<u32>()
        );

        let cache = snapshot.target(node, "cache");
        assert_eq!(snapshot.name(cache), "workerd / Cache");
        assert_eq!(snapshot.size(cache), size_of::<Cache>());
        let entries = snapshot.target(cache, "entries");
        assert_eq!(snapshot.name(entries), "workerd / HashMap");
        assert_eq!(
            snapshot.name(snapshot.target(entries, "key")),
            "workerd / String"
        );
        assert_eq!(
            snapshot.name(snapshot.target(entries, "value")),
            "workerd / String"
        );

        // A handle is an edge into the V8 heap.
        let object = snapshot.target(node, "object");
        assert_eq!(snapshot.nodes[object].kind, "object");

        // A field that holds nothing and a field that is not reported have no edge.
        assert!(snapshot.targets(node, "missing").is_empty());
        assert!(snapshot.targets(node, "unreported").is_empty());

        // The node is linked to its wrapper, as for a C++ resource. V8 shows the wrapper as one
        // node with the cppgc object it holds.
        let wrapper = snapshot.target(node, "native_to_javascript");
        assert!(!snapshot.name(wrapper).starts_with("workerd /"));
        assert!(
            snapshot
                .targets(wrapper, "javascript_to_native")
                .contains(&node)
        );
        Ok(())
    });
}

#[test]
fn resource_without_memory_info_reports_no_fields() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let silent = jsg::Rc::new(
            lock,
            Silent {
                name: "a name".to_owned(),
            },
        );
        expose(lock, ctx, "silent", &silent)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Silent");
        let mut edges = snapshot.edge_names(node);
        edges.sort_unstable();
        assert_eq!(edges, ["cppgcshim", "native_to_javascript"]);
        assert_eq!(
            snapshot.size(node),
            size_of::<Silent>() + crate::Harness::rust_wrappable_size()
        );
        Ok(())
    });
}

#[test]
fn shared_resource_is_one_node() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let shared = new_child(lock);
        let first = parent_of(lock, ctx, shared.clone());
        let second = parent_of(lock, ctx, shared.clone());
        expose(lock, ctx, "first", &first)?;
        expose(lock, ctx, "second", &second)?;
        expose(lock, ctx, "shared", &shared)?;

        let snapshot = Snapshot::take(lock);
        let parents = snapshot.find_all("Parent");
        assert_eq!(parents.len(), 2);
        let first_child = snapshot.target(parents[0], "child");
        let second_child = snapshot.target(parents[1], "child");
        assert_eq!(first_child, second_child);
        Ok(())
    });
}

#[test]
fn cycle_of_resources_is_tracked_once() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let parent = parent(lock, ctx);
        *parent.child.parent.borrow_mut(lock) = Some(parent.clone());
        expose(lock, ctx, "parent", &parent)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Parent");
        let child = snapshot.target(node, "child");
        assert_eq!(snapshot.name(child), "workerd / Child");
        assert_eq!(snapshot.target(child, "parent"), node);

        // Break the cycle, which GC cannot collect while Rust holds a strong reference.
        parent.child.parent.borrow_mut(lock).take();
        Ok(())
    });
}

#[test]
fn function_data_is_a_node() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let function = v8::Function::new(lock, "data".to_owned(), |lock, data, _args| {
            data.clone().to_js(lock)
        })?;
        ctx.set_global(lock, "f", v8::Local::<v8::Value>::from(function));

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("WrappableFunction");
        assert!(snapshot.size(node) > crate::Harness::rust_wrappable_size());
        Ok(())
    });
}

// =============================================================================
// Field types
// =============================================================================

/// A retainer with a retainer at its start, which shares its address.
#[repr(C)]
struct Outer {
    inner: Inner,
    label: String,
}

struct Inner {
    name: String,
}

impl jsg::MemoryRetainer for Outer {
    fn memory_name(&self) -> &'static CStr {
        c"Outer"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"inner", &self.inner);
        tracker.track_field(c"label", &self.label);
    }
}

impl jsg::MemoryRetainer for Inner {
    fn memory_name(&self) -> &'static CStr {
        c"Inner"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"name", &self.name);
    }
}

/// A retainer with no bytes, which has no address of its own.
struct Empty;

impl jsg::MemoryRetainer for Empty {
    fn memory_name(&self) -> &'static CStr {
        c"Empty"
    }

    fn memory_info(&self, _tracker: &mut jsg::MemoryTracker<'_>) {}
}

/// A retainer in a cycle through `std::rc::Rc`, which only its deduplication ends.
struct Link {
    name: String,
    next: RefCell<Option<std::rc::Rc<Self>>>,
}

impl jsg::MemoryRetainer for Link {
    fn memory_name(&self) -> &'static CStr {
        c"Link"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"name", &self.name);
        tracker.track_field(c"next", &self.next);
    }
}

jsg::jsg_memory_retainer!(Outer, Inner, Empty, Link);

jsg::jsg_traced!(Inner);

/// Two links, each the other's `next`.
fn new_ring() -> std::rc::Rc<Link> {
    let first = std::rc::Rc::new(Link {
        name: "first".to_owned(),
        next: RefCell::new(None),
    });
    let second = std::rc::Rc::new(Link {
        name: "second".to_owned(),
        next: RefCell::new(Some(first.clone())),
    });
    *first.next.borrow_mut() = Some(second);
    first
}

fn new_inner(name: &str) -> Inner {
    Inner {
        name: name.to_owned(),
    }
}

/// Holds one field of every built-in kind that tests below look at. Some of them have no `Traced`
/// impl, so it traces its fields itself.
#[jsg_resource(memory_info, custom_trace)]
struct Fields {
    pub outer: Outer,
    pub empty_a: Empty,
    pub empty_b: Empty,
    pub cell_a: jsg::Cell<Inner>,
    pub cell_b: jsg::Cell<Inner>,
    pub inners: Vec<Inner>,
    pub optional_inners: Vec<Option<Inner>>,
    pub boxed_inner: Box<Inner>,
    pub boxed_optional_inner: Box<Option<Inner>>,
    pub shared_a: std::rc::Rc<Cache>,
    pub shared_b: std::rc::Rc<Cache>,
    pub ring: std::rc::Rc<Link>,
    pub boxed_child: Box<Option<jsg::Rc<Child>>>,
    pub boxed_number: Box<u64>,
    pub boxed_str: Box<str>,
    pub boxed_slice: Box<[jsg::Rc<Child>]>,
    pub nested: Vec<Vec<jsg::Rc<Child>>>,
    pub optional_numbers: Vec<Option<u32>>,
    pub deque: VecDeque<String>,
    pub array: [jsg::Rc<Child>; 2],
    pub set: HashSet<jsg::Rc<Child>>,
    pub tree: BTreeMap<u32, String>,
    pub plain_dict: jsg::Dict<u32>,
    pub dict: jsg::Dict<jsg::Rc<Child>>,
    pub weak: jsg::Weak<Child>,
    pub lenient: jsg::Lenient<u32>,
    pub global: v8::Global<v8::Value>,
    pub store: v8::BackingStore,
    pub borrowed: RefCell<String>,
}

#[jsg_resource]
impl Fields {}

impl jsg::Traced for Fields {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        self.boxed_child.trace(visitor);
        for child in &*self.boxed_slice {
            child.trace(visitor);
        }
        self.nested.trace(visitor);
        for child in &self.array {
            child.trace(visitor);
        }
        self.set.trace(visitor);
        self.dict.trace(visitor);
        self.lenient.trace(visitor);
        // `global` is a strong handle, which GC never visits.
    }
}

/// Runs `check` on a snapshot of a `Fields` resource, with its node.
fn with_fields(check: impl FnOnce(&Snapshot, usize, &Fields)) {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let weak_target = new_child(lock);
        let unconvertable = ctx.eval_raw(lock, "({ unconvertable: 1 })").unwrap();
        let global = ctx.eval_raw(lock, "({ global: 1 })").unwrap();
        let (c1, c2) = (new_child(lock), new_child(lock));
        ctx.set_global(lock, "c1", c1);
        ctx.set_global(lock, "c2", c2);
        let dict = ctx.eval_raw(lock, "({ a: c1, b: c2 })").unwrap();
        let dict = jsg::Dict::<jsg::Rc<Child>>::from_js(lock, dict)?;
        let plain_dict = ctx.eval_raw(lock, "({ a: 1, bb: 2 })").unwrap();
        let plain_dict = jsg::Dict::<u32>::from_js(lock, plain_dict)?;
        let shared = std::rc::Rc::new(new_cache());
        let fields = Fields {
            outer: Outer {
                inner: new_inner("inner"),
                label: "label".to_owned(),
            },
            empty_a: Empty,
            empty_b: Empty,
            cell_a: jsg::Cell::new(new_inner("a")),
            cell_b: jsg::Cell::new(new_inner("b")),
            inners: {
                let mut inners = Vec::with_capacity(5);
                inners.extend([new_inner("x"), new_inner("y")]);
                inners
            },
            optional_inners: {
                let mut inners = Vec::with_capacity(4);
                inners.extend([Some(new_inner("some")), None]);
                inners
            },
            boxed_inner: Box::new(new_inner("boxed")),
            boxed_optional_inner: Box::new(Some(new_inner("boxed some"))),
            shared_a: shared.clone(),
            shared_b: shared,
            ring: new_ring(),
            boxed_child: Box::new(Some(new_child(lock))),
            boxed_number: Box::new(7),
            boxed_str: "boxed str".into(),
            boxed_slice: vec![new_child(lock), new_child(lock)].into_boxed_slice(),
            nested: vec![
                vec![new_child(lock)],
                vec![new_child(lock), new_child(lock)],
            ],
            optional_numbers: vec![Some(1), None],
            deque: VecDeque::from(["d".to_owned()]),
            array: [new_child(lock), new_child(lock)],
            set: HashSet::from([new_child(lock)]),
            tree: BTreeMap::from([(1, "one".to_owned())]),
            plain_dict,
            dict,
            weak: weak_target.downgrade(),
            lenient: jsg::Lenient::Unconvertable(v8::TracedGlobal::new(lock, unconvertable)),
            global: global.to_global(lock),
            store: v8::ArrayBuffer::new(lock, &[1u8, 2, 3, 4]).backing_store(),
            borrowed: RefCell::new("borrowed".to_owned()),
        };
        let fields = jsg::Rc::new(lock, fields);
        expose(lock, ctx, "fields", &fields)?;
        expose(lock, ctx, "weakTarget", &weak_target)?;

        // A snapshot taken while a cell is mutably borrowed skips the cell. Taking a snapshot runs
        // a GC first, so the cell holds no GC references, which the GC could not visit.
        let borrow = fields.borrowed.borrow_mut();
        let snapshot = Snapshot::take(lock);
        drop(borrow);
        let node = snapshot.find("Fields");
        check(&snapshot, node, &fields);
        // Break the cycle of links, which would leak.
        fields.ring.next.borrow_mut().take();
        Ok(())
    });
}

#[test]
fn retainer_and_its_first_field_are_two_nodes() {
    with_fields(|snapshot, node, _| {
        let outer = snapshot.target(node, "outer");
        let inner = snapshot.target(outer, "inner");
        assert_eq!(snapshot.name(outer), "workerd / Outer");
        assert_eq!(snapshot.name(inner), "workerd / Inner");
        assert_eq!(
            snapshot.name(snapshot.target(inner, "name")),
            "workerd / String"
        );
        assert_eq!(
            snapshot.name(snapshot.target(outer, "label")),
            "workerd / String"
        );
    });
}

#[test]
fn zero_sized_retainers_are_separate_nodes() {
    with_fields(|snapshot, node, _| {
        let a = snapshot.target(node, "empty_a");
        let b = snapshot.target(node, "empty_b");
        assert_eq!(snapshot.name(a), "workerd / Empty");
        assert_ne!(a, b);
    });
}

#[test]
fn retainers_in_cells_are_separate_nodes() {
    with_fields(|snapshot, node, _| {
        let a = snapshot.target(node, "cell_a");
        let b = snapshot.target(node, "cell_b");
        assert_eq!(snapshot.name(a), "workerd / Inner");
        assert_ne!(a, b);
    });
}

#[test]
fn collection_of_retainers_leaves_their_bytes_to_them() {
    with_fields(|snapshot, node, fields| {
        let inners = snapshot.target(node, "inners");
        // Only the spare capacity is the Vec's own.
        assert!(fields.inners.capacity() > 2);
        assert_eq!(
            snapshot.size(inners),
            (fields.inners.capacity() - 2) * size_of::<Inner>()
        );
        assert_eq!(
            snapshot.target_names(inners),
            ["workerd / Inner", "workerd / Inner"]
        );
        for element in snapshot.targets(inners, "") {
            assert_eq!(snapshot.size(element), size_of::<Inner>());
        }
    });
}

#[test]
fn collection_leaves_bytes_of_retainers_in_wrappers_to_them() {
    with_fields(|snapshot, node, fields| {
        let inners = snapshot.target(node, "optional_inners");
        // One element is a retainer, whose node counts its bytes; the `None` counts in the Vec.
        assert_eq!(
            snapshot.size(inners),
            fields.optional_inners.capacity() * size_of::<Option<Inner>>() - size_of::<Inner>()
        );
        assert_eq!(snapshot.target_names(inners), ["workerd / Inner"]);
    });
}

#[test]
fn boxed_retainer_in_wrapper_is_the_retainers_node() {
    // `Option<Inner>` takes its tag from a niche of `Inner`, so the retainer's node counts all
    // of the allocation.
    assert_eq!(size_of::<Option<Inner>>(), size_of::<Inner>());
    with_fields(|snapshot, node, _| {
        let inner = snapshot.target(node, "boxed_optional_inner");
        assert_eq!(snapshot.name(inner), "workerd / Inner");
        assert_eq!(snapshot.size(inner), size_of::<Inner>());
    });
}

#[test]
fn retainer_shared_through_rc_is_one_node() {
    with_fields(|snapshot, node, _| {
        let a = snapshot.target(node, "shared_a");
        let b = snapshot.target(node, "shared_b");
        assert_eq!(snapshot.name(a), "workerd / Cache");
        assert_eq!(a, b);
        assert_eq!(snapshot.find_all("Cache"), [a]);
    });
}

#[test]
fn cycle_of_retainers_through_rc_is_tracked_once() {
    with_fields(|snapshot, node, _| {
        let first = snapshot.target(node, "ring");
        let second = snapshot.target(first, "next");
        assert_ne!(first, second);
        assert_eq!(snapshot.target(second, "next"), first);
        assert_eq!(snapshot.find_all("Link").len(), 2);
    });
}

#[test]
fn elements_have_empty_edge_names() {
    with_fields(|snapshot, node, _| {
        let resources = snapshot.target(node, "nested");
        let retainers = snapshot.target(node, "inners");
        let element_edges = snapshot
            .edges(resources)
            .into_iter()
            .chain(snapshot.edges(retainers))
            .collect::<Vec<_>>();
        assert_eq!(element_edges.len(), 4);
        for edge in element_edges {
            assert_eq!(edge, ("internal", ""));
        }
    });
}

#[test]
fn boxes_report_what_they_hold() {
    with_fields(|snapshot, node, _| {
        // A boxed retainer is the retainer's node, which counts the allocation.
        let inner = snapshot.target(node, "boxed_inner");
        assert_eq!(snapshot.name(inner), "workerd / Inner");
        assert_eq!(snapshot.size(inner), size_of::<Inner>());

        let child = snapshot.target(node, "boxed_child");
        assert_eq!(snapshot.name(child), "workerd / Box");
        assert_eq!(snapshot.size(child), size_of::<Option<jsg::Rc<Child>>>());
        assert_eq!(snapshot.target_names(child), ["workerd / Child"]);

        let number = snapshot.target(node, "boxed_number");
        assert_eq!(snapshot.name(number), "workerd / Box");
        assert_eq!(snapshot.size(number), size_of::<u64>());

        let string = snapshot.target(node, "boxed_str");
        assert_eq!(snapshot.name(string), "workerd / Box<str>");
        assert_eq!(snapshot.size(string), "boxed str".len());

        let slice = snapshot.target(node, "boxed_slice");
        assert_eq!(snapshot.name(slice), "workerd / Box<[T]>");
        assert_eq!(snapshot.target_names(slice).len(), 2);
    });
}

#[test]
fn nested_collections_report_each_level() {
    with_fields(|snapshot, node, _| {
        let outer = snapshot.target(node, "nested");
        assert_eq!(
            snapshot.target_names(outer),
            ["workerd / Vec", "workerd / Vec"]
        );
        let counts: Vec<_> = snapshot
            .targets(outer, "")
            .into_iter()
            .map(|inner| snapshot.target_names(inner).len())
            .collect();
        assert_eq!(counts, [1, 2]);
    });
}

#[test]
fn other_collections() {
    with_fields(|snapshot, node, fields| {
        // Options of plain data are plain data.
        let optional = snapshot.target(node, "optional_numbers");
        assert!(snapshot.nodes[optional].edges.is_empty());
        assert_eq!(
            snapshot.size(optional),
            fields.optional_numbers.capacity() * size_of::<Option<u32>>()
        );

        let deque = snapshot.target(node, "deque");
        assert_eq!(snapshot.name(deque), "workerd / VecDeque");
        assert_eq!(snapshot.target_names(deque), ["workerd / String"]);

        // An array is inline, so each element is a field of the resource.
        assert_eq!(snapshot.targets(node, "array").len(), 2);

        let set = snapshot.target(node, "set");
        assert_eq!(snapshot.name(set), "workerd / HashSet");
        assert_eq!(snapshot.target_names(set), ["workerd / Child"]);

        let tree = snapshot.target(node, "tree");
        assert_eq!(snapshot.name(tree), "workerd / BTreeMap");
        assert_eq!(snapshot.edge_names(tree), ["value"]);

        // A dict of plain data is one node, keys included.
        let plain_dict = snapshot.target(node, "plain_dict");
        assert!(snapshot.nodes[plain_dict].edges.is_empty());
        assert!(snapshot.size(plain_dict) >= 2 * size_of::<(String, u32)>() + "abb".len());

        let dict = snapshot.target(node, "dict");
        assert_eq!(snapshot.name(dict), "workerd / Dict");
        let mut edges = snapshot.edge_names(dict);
        edges.sort_unstable();
        assert_eq!(edges, ["key", "key", "value", "value"]);
    });
}

#[test]
fn handles_and_buffers() {
    with_fields(|snapshot, node, _| {
        assert!(snapshot.targets(node, "weak").is_empty());

        let lenient = snapshot.target(node, "lenient");
        assert_eq!(snapshot.nodes[lenient].kind, "object");

        let global = snapshot.target(node, "global");
        assert_eq!(snapshot.nodes[global].kind, "object");

        let store = snapshot.target(node, "store");
        assert_eq!(snapshot.name(store), "workerd / BackingStore");
        assert_eq!(snapshot.size(store), 4);

        // The cell was mutably borrowed while the snapshot was taken.
        assert!(snapshot.targets(node, "borrowed").is_empty());
    });
}

/// A heap snapshot of one isolate leaves alone a handle and a resource of another that a
/// resource of the first holds.
#[test]
fn handle_and_resource_of_another_isolate_are_not_reported() {
    // A `Global` has no `Traced` impl, and is never visited; nor is `resource`, which GC would
    // visit in the wrong isolate.
    #[jsg_resource(memory_info, custom_trace)]
    struct Holder {
        handle: RefCell<Option<v8::Global<v8::Value>>>,
        resource: RefCell<Option<jsg::Rc<Child>>>,
    }

    impl jsg::Traced for Holder {
        fn trace(&self, _: &mut jsg::GcVisitor) {}
    }

    #[jsg_resource]
    impl Holder {}

    let a = crate::Harness::new();
    let b = crate::Harness::new();
    let mut global = None;
    let mut resource = None;
    a.run_in_context(|lock, ctx| {
        let object: v8::Local<'_, v8::Value> = lock.new_object().into();
        global = Some(object.to_global(lock));
        let child = new_child(lock);
        expose(lock, ctx, "child", &child)?;
        resource = Some(child);
        Ok(())
    });
    let mut holder = None;
    b.run_in_context(|lock, ctx| {
        let held = jsg::Rc::new(
            lock,
            Holder {
                handle: RefCell::new(global.take()),
                resource: RefCell::new(resource.take()),
            },
        );
        expose(lock, ctx, "holder", &held)?;
        holder = Some(held);
        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Holder");
        assert!(snapshot.targets(node, "handle").is_empty());
        assert!(snapshot.targets(node, "resource").is_empty());
        Ok(())
    });

    let holder = holder.expect("set above");
    // Each handle and resource is released under the lock of its own isolate.
    a.run_in_context(|_lock, _ctx| {
        drop(holder.handle.borrow_mut().take().expect("set above"));
        drop(holder.resource.borrow_mut().take().expect("set above"));
        Ok(())
    });
    b.run_in_context(move |_lock, _ctx| {
        drop(holder);
        Ok(())
    });
}

// =============================================================================
// Promises, async context frames and rest arguments
// =============================================================================

#[jsg_resource(memory_info)]
struct Pending {
    pub promise: jsg::TracedPromise<u32>,
    pub resolver: jsg::TracedPromiseResolver<u32>,
    pub frame: jsg::TracedAsyncContextFrame,
    pub root_frame: jsg::TracedAsyncContextFrame,
    pub arguments: jsg::Arguments<String>,
}

#[jsg_resource]
impl Pending {}

#[test]
fn promises_and_frames_are_edges_into_the_v8_heap() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let (promise, resolver) = jsg::Promise::<u32>::new_with_resolver(lock)?;
        let promise = promise.into_traced(lock);
        let resolver = resolver.into_traced(lock);
        let stored = "stored".to_owned().to_js(lock)?;
        let frame = crate::Harness::run_in_async_context(
            lock,
            stored,
            jsg::TracedAsyncContextFrame::current,
        );
        assert!(!frame.is_root());
        let pending = Pending {
            promise,
            resolver,
            frame,
            root_frame: jsg::TracedAsyncContextFrame::root(),
            arguments: vec!["a".to_owned(), "b".to_owned()].into(),
        };
        let pending = jsg::Rc::new(lock, pending);
        expose(lock, ctx, "pending", &pending)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Pending");
        for edge in ["promise", "resolver"] {
            let target = snapshot.target(node, edge);
            assert_eq!(snapshot.nodes[target].kind, "object", "{edge}");
        }
        snapshot.target(node, "frame");
        // The root frame has no wrapper.
        assert!(snapshot.targets(node, "root_frame").is_empty());

        let arguments = snapshot.target(node, "arguments");
        assert_eq!(snapshot.name(arguments), "workerd / Vec");
        assert_eq!(
            snapshot.target_names(arguments),
            ["workerd / String", "workerd / String"]
        );
        Ok(())
    });
}

// =============================================================================
// Shared pointers and std cells
// =============================================================================

#[jsg_resource(memory_info, custom_trace)]
struct Shared {
    pub number: std::rc::Rc<u64>,
    pub string: std::sync::Arc<String>,
    pub copied: Cell<u32>,
}

impl jsg::Traced for Shared {
    fn trace(&self, _: &mut jsg::GcVisitor) {}
}

#[jsg_resource]
impl Shared {}

#[test]
fn shared_pointers_count_their_allocations() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let shared = Shared {
            number: std::rc::Rc::new(7),
            string: std::sync::Arc::new("a string".to_owned()),
            copied: Cell::new(1),
        };
        let shared = jsg::Rc::new(lock, shared);
        expose(lock, ctx, "shared", &shared)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Shared");

        // Plain data is a node of its size.
        let number = snapshot.target(node, "number");
        assert_eq!(snapshot.name(number), "workerd / Rc");
        assert_eq!(snapshot.size(number), size_of::<u64>());
        assert!(snapshot.nodes[number].edges.is_empty());

        // Anything else is a node of the bytes it leaves uncounted, which it reports to.
        let string = snapshot.target(node, "string");
        assert_eq!(snapshot.name(string), "workerd / Arc");
        assert_eq!(snapshot.size(string), size_of::<String>());
        assert_eq!(snapshot.target_names(string), ["workerd / String"]);

        // A std cell reports nothing, as plain data does.
        assert!(snapshot.targets(node, "copied").is_empty());
        Ok(())
    });
}

/// A value that is not a retainer, two of whose pointers share each allocation: a ladder that
/// reporting each pointer in full would visit `2^RUNGS` times.
struct Rung {
    left: Option<std::rc::Rc<Self>>,
    right: Option<std::rc::Rc<Self>>,
}

impl jsg::MemoryInfo for Rung {
    fn track_as_field(
        &self,
        tracker: &mut jsg::MemoryTracker<'_>,
        edge_name: Option<&'static CStr>,
    ) {
        self.left.track_as_field(tracker, edge_name);
        self.right.track_as_field(tracker, edge_name);
    }
}

const RUNGS: usize = 64;

#[jsg_resource(memory_info, custom_trace)]
struct Ladder {
    pub top: std::rc::Rc<Rung>,
    pub number: std::rc::Rc<u64>,
    pub same_number: std::rc::Rc<u64>,
}

impl jsg::Traced for Ladder {
    fn trace(&self, _: &mut jsg::GcVisitor) {}
}

#[jsg_resource]
impl Ladder {}

#[test]
fn allocation_shared_through_rc_is_one_node() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let mut top = std::rc::Rc::new(Rung {
            left: None,
            right: None,
        });
        for _ in 1..RUNGS {
            top = std::rc::Rc::new(Rung {
                left: Some(top.clone()),
                right: Some(top),
            });
        }
        let number = std::rc::Rc::new(7);
        let ladder = Ladder {
            top,
            same_number: number.clone(),
            number,
        };
        let ladder = jsg::Rc::new(lock, ladder);
        expose(lock, ctx, "ladder", &ladder)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Ladder");
        // One node for each rung's allocation, and one for the number's.
        assert_eq!(snapshot.find_all("Rc").len(), RUNGS + 1);
        let top = snapshot.target(node, "top");
        let next = snapshot.targets(top, "");
        assert_eq!(next.len(), 2);
        assert_eq!(next[0], next[1]);
        assert_eq!(
            snapshot.target(node, "number"),
            snapshot.target(node, "same_number")
        );
        Ok(())
    });
}

// =============================================================================
// Depth
// =============================================================================

/// A retainer in a linked list, which reporting follows link by link.
struct ListNode {
    next: Option<Box<Self>>,
}

impl jsg::MemoryRetainer for ListNode {
    fn memory_name(&self) -> &'static CStr {
        c"ListNode"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"next", &self.next);
    }
}

jsg::jsg_memory_retainer!(ListNode);

impl Drop for ListNode {
    // Drops the list link by link, since a recursive drop of a long list overflows the stack.
    fn drop(&mut self) {
        let mut next = self.next.take();
        while let Some(mut node) = next {
            next = node.next.take();
        }
    }
}

fn new_list(len: usize) -> ListNode {
    let mut head = ListNode { next: None };
    for _ in 1..len {
        head = ListNode {
            next: Some(Box::new(head)),
        };
    }
    head
}

/// A list of retainers, of which the last holds `payload`.
struct Tail {
    next: Option<Box<Self>>,
    payload: Option<std::rc::Rc<Inner>>,
}

impl jsg::MemoryRetainer for Tail {
    fn memory_name(&self) -> &'static CStr {
        c"Tail"
    }

    fn memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"next", &self.next);
        tracker.track_field(c"payload", &self.payload);
    }
}

jsg::jsg_memory_retainer!(Tail);

impl Drop for Tail {
    // Drops the list link by link, since a recursive drop of a long list overflows the stack.
    fn drop(&mut self) {
        let mut next = self.next.take();
        while let Some(mut node) = next {
            next = node.next.take();
        }
    }
}

/// A value that is not a retainer, in a cycle through `std::rc::Rc`.
struct Loop {
    next: RefCell<Option<std::rc::Rc<Self>>>,
}

impl jsg::MemoryInfo for Loop {
    fn track_as_field(
        &self,
        tracker: &mut jsg::MemoryTracker<'_>,
        edge_name: Option<&'static CStr>,
    ) {
        self.next.track_as_field(tracker, edge_name);
    }
}

#[jsg_resource(memory_info, custom_trace)]
struct Deep {
    pub list: ListNode,
    pub ring: std::rc::Rc<Loop>,
}

impl jsg::Traced for Deep {
    fn trace(&self, _: &mut jsg::GcVisitor) {}
}

#[jsg_resource]
impl Deep {}

/// Whether `count` nodes nested in a resource's node reach `MAX_DEPTH` and stop there: the
/// resource is a node or two deep, depending on how the snapshot reached it.
fn stops_at_max_depth(count: usize) -> bool {
    let max = jsg::MemoryTracker::MAX_DEPTH;
    (max - 4..=max).contains(&count)
}

#[test]
fn reporting_stops_at_max_depth() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let ring = std::rc::Rc::new(Loop {
            next: RefCell::new(None),
        });
        *ring.next.borrow_mut() = Some(ring.clone());
        let deep = Deep {
            list: new_list(10_000),
            ring,
        };
        let deep = jsg::Rc::new(lock, deep);
        expose(lock, ctx, "deep", &deep)?;

        let snapshot = Snapshot::take(lock);
        let lists = snapshot.find_all("ListNode").len();
        assert!(stops_at_max_depth(lists), "{lists} list nodes");
        // The allocation of the cycle is one node, which points to itself.
        let ring = snapshot.find("Rc");
        assert_eq!(snapshot.targets(ring, ""), [ring]);

        // Break the cycle, which would leak.
        deep.ring.next.borrow_mut().take();
        Ok(())
    });
}

/// Reports each payload first at the end of a list, then as a field of its own. The lists end
/// around `MAX_DEPTH`, so that one of the payloads is first reached at it.
#[jsg_resource(memory_info, custom_trace)]
struct DeepFirst {
    pub lists: Vec<Tail>,
    pub payloads: Vec<std::rc::Rc<Inner>>,
}

impl jsg::Traced for DeepFirst {
    fn trace(&self, _: &mut jsg::GcVisitor) {}
}

#[jsg_resource]
impl DeepFirst {}

#[test]
fn retainer_reached_past_max_depth_is_reported_in_full_from_closer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let max = jsg::MemoryTracker::MAX_DEPTH;
        let mut lists = Vec::new();
        let mut payloads = Vec::new();
        for len in max - 8..=max {
            let payload = std::rc::Rc::new(new_inner("payload"));
            let mut list = Tail {
                next: None,
                payload: Some(payload.clone()),
            };
            for _ in 1..len {
                list = Tail {
                    next: Some(Box::new(list)),
                    payload: None,
                };
            }
            lists.push(list);
            payloads.push(payload);
        }
        let deep = jsg::Rc::new(lock, DeepFirst { lists, payloads });
        expose(lock, ctx, "deep", &deep)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("DeepFirst");
        let payloads = snapshot.targets(snapshot.target(node, "payloads"), "");
        assert_eq!(payloads.len(), 9);
        for payload in payloads {
            assert_eq!(snapshot.name(payload), "workerd / Inner");
            assert_eq!(
                snapshot.name(snapshot.target(payload, "name")),
                "workerd / String"
            );
        }
        Ok(())
    });
}

#[jsg_resource(memory_info)]
struct Chain {
    pub next: Option<jsg::Rc<Self>>,
}

#[jsg_resource]
impl Chain {}

#[test]
fn reporting_a_chain_of_resources_stops_at_max_depth() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let mut head = None;
        for _ in 0..3 * jsg::MemoryTracker::MAX_DEPTH {
            head = Some(jsg::Rc::new(lock, Chain { next: head }));
        }
        expose(lock, ctx, "head", head.as_ref().expect("built above"))?;

        let snapshot = Snapshot::take(lock);
        let links = snapshot.find_all("Chain").len();
        assert!(stops_at_max_depth(links), "{links} resource nodes");
        Ok(())
    });
}

// =============================================================================
// Custom reporting
// =============================================================================

/// A resource that reports its fields itself.
#[jsg_resource(custom_memory_info)]
struct Custom {
    pub entries: Vec<String>,
    pub count: u32,
}

#[jsg_resource]
impl Custom {}

impl jsg::VisitForMemoryInfo for Custom {
    fn visit_for_memory_info(&self, tracker: &mut jsg::MemoryTracker<'_>) {
        tracker.track_field(c"cached_entries", &self.entries);
    }
}

#[test]
fn custom_memory_info_reports_what_its_impl_reports() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let custom = Custom {
            entries: vec!["entry".to_owned()],
            count: 1,
        };
        let custom = jsg::Rc::new(lock, custom);
        expose(lock, ctx, "custom", &custom)?;

        let snapshot = Snapshot::take(lock);
        let node = snapshot.find("Custom");
        let entries = snapshot.target(node, "cached_entries");
        assert_eq!(snapshot.target_names(entries), ["workerd / String"]);
        assert!(snapshot.targets(node, "entries").is_empty());
        Ok(())
    });
}
