// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A simulation of one object served by a cluster's nodes over a bucket
//! whose every call may fail, before or after it applies. Requests write
//! the object's root and facet databases as `SQLite` does and commit;
//! facets are created, deleted, reset and cloned; nodes tier, evict, crash
//! and outlive their leases, and others take the object over. Whatever a
//! seed does:
//!
//! - an epoch inherits every lane as of one point of the epoch it
//!   recovers, at or past that epoch's last acknowledged commit;
//! - no commit is acknowledged once own.json names a later epoch;
//! - once faults stop, the object is taken over again.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::rc::Rc;

use crate::Cluster;
use crate::Layout;
use crate::Route;
use crate::cluster::Setup;
use crate::dst::Harness;
use crate::dst::Rng;
use crate::dst::Store;
use crate::log::DB;
use crate::log::facet_prefix;

const NS: &str = "ns";
const ACTOR: &str = "o";
const LEASE: u64 = 10;
const PAGE: usize = 512;
const NAMES: [&str; 3] = ["a", "b", "%"];

/// Every database's contents by lane.
type Lanes = BTreeMap<String, Vec<u8>>;

/// One open database: its file, and the committed frames of its WAL since
/// the last checkpoint.
struct Db {
    path: PathBuf,
    file: Vec<u8>,
    wal: Vec<(u32, Vec<u8>)>,
    pages: u32,
    /// Where the next WAL write goes; 0 before the WAL header.
    offset: u64,
}

impl Db {
    /// A new database: `SQLite` writes its first page to the file.
    fn new(path: PathBuf, rng: &mut Rng) -> Self {
        Self::open(path, page(rng))
    }

    fn open(path: PathBuf, file: Vec<u8>) -> Self {
        std::fs::write(&path, &file).unwrap();
        let pages = (file.len() / PAGE) as u32;
        Self {
            path,
            file,
            wal: Vec::new(),
            pages,
            offset: 0,
        }
    }

    /// What a connection reads.
    fn state(&self) -> Vec<u8> {
        let mut db = self.file.clone();
        for (number, data) in &self.wal {
            let at = (*number as usize - 1) * PAGE;
            if db.len() < at + PAGE {
                db.resize(at + PAGE, 0);
            }
            db[at..at + PAGE].copy_from_slice(data);
        }
        db.resize(self.pages as usize * PAGE, 0);
        db
    }

    /// The WAL folded into the file, as a checkpoint or the last
    /// connection's close leaves it.
    fn checkpoint(&mut self) {
        self.file = self.state();
        self.wal.clear();
        self.offset = 0;
        std::fs::write(&self.path, &self.file).unwrap();
    }

    /// One transaction, its frames reported as `SQLite` writes them.
    fn commit(&mut self, cluster: &Cluster, rng: &mut Rng) {
        let old = self.pages;
        let pages = match rng.below(10) {
            0 => old.saturating_sub(1).max(1),
            1..4 => old + 1,
            _ => old,
        };
        let mut written = Vec::new();
        if pages > old {
            written.push((pages, page(rng)));
        }
        for _ in 0..=rng.below(2) {
            written.push((1 + rng.below(pages as usize) as u32, page(rng)));
        }
        let wal = format!("{}-wal", self.path.display());
        let fresh = self.offset == 0;
        let mut write = |data: &[u8]| {
            cluster.wal_write(NS, &wal, self.offset, data);
            self.offset += data.len() as u64;
        };
        if fresh {
            let mut header = vec![0; 32];
            header[8..12].copy_from_slice(&(PAGE as u32).to_be_bytes());
            write(&header);
        }
        for (k, (number, data)) in written.iter().enumerate() {
            let mut header = vec![0; 24];
            header[0..4].copy_from_slice(&number.to_be_bytes());
            let size = if k + 1 == written.len() { pages } else { 0 };
            header[4..8].copy_from_slice(&size.to_be_bytes());
            write(&[header, data.clone()].concat());
        }
        self.wal.extend(written);
        self.wal.retain(|(n, _)| *n <= pages);
        self.file.truncate(pages as usize * PAGE);
        self.pages = pages;
    }
}

fn page(rng: &mut Rng) -> Vec<u8> {
    vec![rng.next() as u8; PAGE]
}

/// A node's files: each lane's database numbered as first placed, as the
/// server's facet index numbers facets.
struct Files {
    dir: PathBuf,
    placed: RefCell<BTreeMap<String, PathBuf>>,
}

impl Layout for Files {
    fn place(&self, lane: &str) -> crate::Result<PathBuf> {
        let mut placed = self.placed.borrow_mut();
        let n = placed.len();
        let path = placed.entry(lane.to_owned()).or_insert_with(|| {
            let name = if lane == DB {
                format!("{ACTOR}.sqlite")
            } else {
                format!("{ACTOR}.{n}.sqlite")
            };
            self.dir.join(name)
        });
        Ok(path.clone())
    }

    fn clear(&self) -> crate::Result<()> {
        self.placed.borrow_mut().clear();
        std::fs::remove_dir_all(&self.dir)?;
        std::fs::create_dir_all(&self.dir)?;
        Ok(())
    }
}

impl Files {
    fn lanes(&self) -> Lanes {
        let placed = self.placed.borrow();
        let read =
            |(lane, path): (&String, &PathBuf)| Some((lane.clone(), std::fs::read(path).ok()?));
        placed.iter().filter_map(read).collect()
    }
}

/// The object's container on a node: its ownership's epoch, and its
/// databases once taken over.
struct Container {
    id: u64,
    epoch: u64,
    dbs: Option<BTreeMap<String, Db>>,
    /// Commits in flight: an idle container has none.
    syncing: usize,
}

#[expect(clippy::struct_excessive_bools)]
struct Node {
    up: bool,
    joined: bool,
    lease_until: u64,
    leasing: bool,
    tiering: bool,
    container: Option<Container>,
}

#[derive(Default)]
struct World {
    now: u64,
    nodes: Vec<Node>,
    /// Each opened epoch's states in capture order, the first inherited.
    hist: BTreeMap<u64, Vec<Lanes>>,
    /// The last acknowledged state of each epoch, an index into `hist`.
    acked: BTreeMap<u64, usize>,
    containers: u64,
    /// Ownerships dropped, whose evictions `run` spawns.
    evictions: Vec<(usize, u64)>,
    /// Takeovers that opened a new epoch.
    opened: usize,
    trace: Vec<String>,
    violations: Vec<String>,
}

/// own.json: the owner's epoch, and the last epoch that opened.
fn own(store: &Store) -> Option<(u64, u64)> {
    let body = store.peek(&format!("{}/own.json", crate::object_prefix(NS, ACTOR)))?;
    let v: serde_json::Value = serde_json::from_slice(&body).unwrap();
    Some((v["epoch"].as_u64().unwrap(), v["from"].as_u64().unwrap()))
}

impl World {
    fn trace(&mut self, line: &str) {
        let now = self.now;
        self.trace.push(format!("t{now} {line}"));
    }

    fn container(&mut self, n: usize, id: u64) -> Option<&mut Container> {
        self.nodes[n].container.as_mut().filter(|c| c.id == id)
    }

    /// Drops node `n`'s container `id`, its databases closed, as an abort
    /// or idle eviction does: the ownership goes, and its eviction runs.
    fn release(&mut self, n: usize, id: u64, cluster: &Cluster) {
        let Some(c) = self.nodes[n].container.take_if(|c| c.id == id) else {
            return;
        };
        for mut db in c.dbs.into_iter().flat_map(BTreeMap::into_values) {
            db.checkpoint();
        }
        cluster.close_actor(NS, ACTOR);
        self.evictions.push((n, c.epoch));
        self.trace(&format!("release n{n} epoch {}", c.epoch));
    }

    /// Epoch `epoch`, claimed recovering `from`, took the object over on
    /// `n`, or failed to: what it inherited, if it opened, must be a state
    /// of the epoch it recovers at or past the last acknowledged.
    fn took_over(
        &mut self,
        n: usize,
        epoch: u64,
        from: u64,
        lanes: Lanes,
        ok: bool,
        store: &Store,
    ) {
        if let Some(states) = self.hist.get(&epoch) {
            // Resumed where it was evicted, or failed to.
            if ok && from == epoch && lanes != *states.last().unwrap() {
                let v = format!("n{n} resumed epoch {epoch} not as it left it");
                self.violations.push(v);
            }
            return;
        }
        if !own(store).is_some_and(|(e, f)| e == epoch && f == epoch) {
            return;
        }
        // A new object, opened at its claim, recovers nothing.
        let source = if from == epoch { 0 } else { from };
        let empty = vec![Lanes::new()];
        let states = self.hist.get(&source).unwrap_or(&empty);
        let acked = self.acked.get(&source).copied().unwrap_or(0);
        if !states[acked..].contains(&lanes) {
            let s = states.iter().position(|s| *s == lanes);
            self.violations.push(format!(
                "epoch {epoch} on n{n} inherited epoch {source} as of {s:?} \
                 of {}, acked {acked}: {:?}",
                states.len(),
                lanes.iter().map(|(l, d)| (l, d.len())).collect::<Vec<_>>()
            ));
        }
        self.trace(&format!("open epoch {epoch} on n{n} from {source}"));
        self.hist.insert(epoch, vec![lanes]);
        self.opened += 1;
    }
}

struct Sim {
    dst: Harness,
    world: Rc<RefCell<World>>,
    clusters: Vec<Rc<Cluster>>,
    files: Vec<Rc<Files>>,
    root: PathBuf,
    faults: bool,
}

impl Sim {
    fn new(seed: u64) -> Self {
        let tmp = std::env::var_os("TEST_TMPDIR").map_or_else(std::env::temp_dir, PathBuf::from);
        let root = tmp.join(format!("owners-{seed}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let mut sim = Self {
            dst: Harness::new(seed),
            world: Rc::default(),
            clusters: Vec::new(),
            files: Vec::new(),
            root,
            faults: true,
        };
        sim.world.borrow_mut().hist.insert(0, vec![Lanes::new()]);
        for _ in 0..3 {
            sim.start();
        }
        sim
    }

    fn trace(&self, line: &str) {
        self.world.borrow_mut().trace(line);
    }

    fn start(&mut self) {
        let n = self.dst.node();
        let node = format!("n{n}");
        let dir = self.root.join(&node);
        std::fs::create_dir_all(&dir).unwrap();
        let setup = Setup {
            url: String::new(),
            options: Vec::new(),
            address: format!("{node}:1"),
            node,
            ttl_ms: LEASE,
            key: Vec::new(),
        };
        self.clusters
            .push(Cluster::over(self.dst.bucket.clone(), setup));
        let placed = RefCell::default();
        self.files.push(Rc::new(Files { dir, placed }));
        self.world.borrow_mut().nodes.push(Node {
            up: true,
            joined: false,
            lease_until: 0,
            leasing: false,
            tiering: false,
            container: None,
        });
        self.trace(&format!("start n{n}"));
    }

    fn crash(&mut self, n: usize) {
        self.dst.crash(n);
        let mut world = self.world.borrow_mut();
        world.nodes[n].up = false;
        world.nodes[n].container = None;
        world.trace(&format!("crash n{n}"));
    }

    fn up(&self) -> Vec<usize> {
        let world = self.world.borrow();
        (0..world.nodes.len())
            .filter(|&n| world.nodes[n].up)
            .collect()
    }

    /// Nodes that noticed their lease lapsed stop, and start again.
    fn lapse(&mut self) {
        for n in self.up() {
            let lapsed = {
                let world = self.world.borrow();
                let node = &world.nodes[n];
                node.joined && world.now >= node.lease_until
            };
            if lapsed {
                self.trace(&format!("lapse n{n}"));
                self.crash(n);
                self.start();
            }
        }
    }

    fn lease(&mut self, n: usize) {
        let cluster = Rc::clone(&self.clusters[n]);
        let world = Rc::clone(&self.world);
        let now = {
            let mut world = world.borrow_mut();
            if std::mem::replace(&mut world.nodes[n].leasing, true) {
                return;
            }
            world.now
        };
        self.dst.spawn(n, async move {
            let renewed = cluster.renew(now).await;
            let mut world = world.borrow_mut();
            let node = &mut world.nodes[n];
            if let Ok(until) = renewed {
                node.lease_until = until;
            }
            node.joined = true;
            node.leasing = false;
        });
    }

    fn tier(&mut self, n: usize) {
        let cluster = Rc::clone(&self.clusters[n]);
        let world = Rc::clone(&self.world);
        if std::mem::replace(&mut world.borrow_mut().nodes[n].tiering, true) {
            return;
        }
        self.dst.spawn(n, async move {
            cluster.tier().await;
            world.borrow_mut().nodes[n].tiering = false;
        });
    }

    /// A request on `n`: run where the object is open, else claim it.
    fn request(&mut self, n: usize) {
        let ready = {
            let world = self.world.borrow();
            let c = world.nodes[n].container.as_ref();
            c.map(|c| c.dbs.is_some())
        };
        match ready {
            Some(true) => self.write(n),
            Some(false) => {}
            None => self.claim(n),
        }
    }

    /// Resolve the object on `n`, and take it over if `n` owns it, as one
    /// cold resolution does while the node's lease holds.
    fn claim(&mut self, n: usize) {
        let cluster = Rc::clone(&self.clusters[n]);
        let files = Rc::clone(&self.files[n]);
        let world = Rc::clone(&self.world);
        let store = Rc::clone(&self.dst.store);
        let (id, now) = {
            let mut world = world.borrow_mut();
            let now = world.now;
            if now >= world.nodes[n].lease_until {
                return;
            }
            world.containers += 1;
            let id = world.containers;
            world.nodes[n].container = Some(Container {
                id,
                epoch: 0,
                dbs: None,
                syncing: 0,
            });
            (id, now)
        };
        self.dst.spawn(n, async move {
            let route = cluster.acquire_or_route(NS, ACTOR, now).await;
            let epoch = match route {
                Ok(Route::Owned(epoch)) => epoch,
                route => {
                    let mut world = world.borrow_mut();
                    let why = match route {
                        Err(e) => e.to_string(),
                        _ => "remote".into(),
                    };
                    world.trace(&format!("claim n{n}: {why}"));
                    world.nodes[n].container.take_if(|c| c.id == id);
                    return;
                }
            };
            let (claimed, from) = own(&store).unwrap();
            assert_eq!(claimed, epoch);
            world.borrow_mut().container(n, id).unwrap().epoch = epoch;
            let restored = cluster.restore_actor(NS, ACTOR, epoch, &*files).await;
            let mut world = world.borrow_mut();
            world.trace(&format!(
                "take over n{n} epoch {epoch} from {from}: {:?}",
                restored.as_ref().err().map(ToString::to_string)
            ));
            world.took_over(n, epoch, from, files.lanes(), restored.is_ok(), &store);
            if restored.is_err() {
                return world.release(n, id, &cluster);
            }
            let placed = files.placed.borrow();
            let open = |(lane, data): (String, Vec<u8>)| {
                let db = Db::open(placed[&lane].clone(), data);
                (lane, db)
            };
            let dbs = files.lanes().into_iter().map(open).collect();
            world.container(n, id).unwrap().dbs = Some(dbs);
        });
    }

    /// One request on `n`'s open container: its writes, then its commit.
    fn write(&mut self, n: usize) {
        let cluster = Rc::clone(&self.clusters[n]);
        let files = Rc::clone(&self.files[n]);
        let (id, epoch, mut dbs) = {
            let mut world = self.world.borrow_mut();
            let c = world.nodes[n].container.as_mut().unwrap();
            (c.id, c.epoch, c.dbs.take().unwrap())
        };
        let (what, wrote) = op(&mut self.dst.rng, &cluster, &files, &mut dbs);
        let state: Lanes = dbs.iter().map(|(l, d)| (l.clone(), d.state())).collect();
        let idx = {
            let mut world = self.world.borrow_mut();
            world.trace(&format!("n{n} epoch {epoch}: {what}"));
            let c = world.container(n, id).unwrap();
            c.dbs = Some(dbs);
            if !wrote {
                return;
            }
            c.syncing += 1;
            let states = world.hist.get_mut(&epoch).unwrap();
            if *states.last().unwrap() != state {
                states.push(state);
            }
            states.len() - 1
        };
        let world = Rc::clone(&self.world);
        let store = Rc::clone(&self.dst.store);
        self.dst.spawn(n, async move {
            let synced = cluster.sync_actor(NS, ACTOR).await;
            let mut world = world.borrow_mut();
            if let Some(c) = world.container(n, id) {
                c.syncing -= 1;
            }
            match synced {
                Ok(()) => {
                    let acked = world.acked.entry(epoch).or_default();
                    let more = idx > *acked;
                    *acked = idx.max(*acked);
                    let named = own(&store).map(|o| o.0);
                    if more && named != Some(epoch) {
                        world.violations.push(format!(
                            "epoch {epoch} acknowledged {idx} on n{n} with own.json at {named:?}"
                        ));
                    }
                    world.trace(&format!("ack n{n} epoch {epoch} {idx}"));
                }
                Err(e) => {
                    world.trace(&format!("commit n{n} epoch {epoch} {idx}: {e}"));
                    world.release(n, id, &cluster);
                }
            }
        });
    }

    /// Workerd evicts `n`'s container once it is idle.
    fn evict(&self, n: usize) {
        let mut world = self.world.borrow_mut();
        let c = world.nodes[n].container.as_ref();
        if let Some(c) = c.filter(|c| c.dbs.is_some() && c.syncing == 0) {
            let id = c.id;
            world.release(n, id, &self.clusters[n]);
        }
    }

    /// Drive every task until each waits on the store, evictions included.
    fn run(&mut self) {
        self.dst.run();
        let evictions = std::mem::take(&mut self.world.borrow_mut().evictions);
        for (n, epoch) in evictions {
            let cluster = Rc::clone(&self.clusters[n]);
            self.dst.spawn(n, async move {
                cluster.evict_actor(NS, ACTOR, epoch).await;
            });
        }
        self.dst.run();
    }

    fn step(&mut self) {
        self.run();
        let up = self.up();
        let any = up[self.dst.rng.below(up.len())];
        // Most requests reach the owner, as routing sends them there.
        let open = {
            let world = self.world.borrow();
            let open = |&&n: &&usize| world.nodes[n].container.is_some();
            up.iter().find(open).copied()
        };
        let to = open.filter(|_| self.dst.rng.chance(80)).unwrap_or(any);
        match self.dst.rng.below(100) {
            0..40 if self.dst.pending() > 0 => {
                let faults = self.faults && self.dst.rng.chance(50);
                let decided = self.dst.decide(faults);
                self.trace(&decided);
            }
            40..65 => self.request(to),
            65..68 => self.evict(to),
            68..73 => self.tier(to),
            73..83 => self.lease(any),
            83..84 if up.len() > 1 => {
                self.crash(any);
                self.start();
            }
            84..85 => self.lapse(),
            _ => self.world.borrow_mut().now += 1,
        }
    }

    /// Stop faults, restart every node, and run until the object is taken
    /// over again and commits.
    fn settle(&mut self) -> bool {
        self.faults = false;
        self.trace("faults stop");
        for n in self.up() {
            self.crash(n);
            self.start();
        }
        let opened = self.world.borrow().opened;
        for _ in 0..200 {
            loop {
                self.run();
                if self.dst.pending() == 0 {
                    break;
                }
                let decided = self.dst.decide(false);
                self.trace(&decided);
            }
            if self.world.borrow().opened > opened {
                return true;
            }
            self.lapse();
            for n in self.up() {
                self.lease(n);
            }
            self.run();
            let up = self.up();
            let n = up[self.dst.rng.below(up.len())];
            self.request(n);
            self.world.borrow_mut().now += 2;
        }
        false
    }
}

/// One request's writes to `dbs`: a commit, or a facet created, deleted,
/// reset or cloned, then written. Answers what it did, and whether it
/// wrote: a request that did not commits nothing.
fn op(
    rng: &mut Rng,
    cluster: &Cluster,
    files: &Files,
    dbs: &mut BTreeMap<String, Db>,
) -> (String, bool) {
    if !dbs.contains_key(DB) {
        let db = Db::new(files.place(DB).unwrap(), rng);
        dbs.entry(DB.into()).or_insert(db).commit(cluster, rng);
        return ("create the root".into(), true);
    }
    let lanes: Vec<String> = dbs.keys().cloned().collect();
    let lane = lanes[rng.below(lanes.len())].clone();
    let prefix = lane.strip_suffix(DB).unwrap().to_owned();
    let remove = |dbs: &mut BTreeMap<String, Db>, doomed: &dyn Fn(&str) -> bool| {
        dbs.retain(|l, db| {
            if doomed(l) {
                let _ = std::fs::remove_file(&db.path);
            }
            !doomed(l)
        });
    };
    let name = NAMES[rng.below(NAMES.len())];
    match rng.below(100) {
        0..12 if prefix.matches('/').count() < 2 => {
            let new = prefix + &facet_prefix([name]) + DB;
            if dbs.contains_key(&new) {
                return (format!("{new} exists"), false);
            }
            let path = files.place(&new).unwrap();
            let path_ = path.display().to_string();
            cluster.open_lane(NS, ACTOR, &path_, &new).unwrap();
            let db = dbs.entry(new.clone()).or_insert_with(|| Db::new(path, rng));
            db.commit(cluster, rng);
            (format!("new {new}"), true)
        }
        12..20 if !prefix.is_empty() => {
            remove(dbs, &|l| l.starts_with(&prefix));
            cluster.delete_lanes(NS, ACTOR, &prefix).unwrap();
            (format!("delete {prefix}"), true)
        }
        20..28 => {
            let path = dbs[&lane].path.clone();
            cluster.database_reset(NS, &path.display().to_string());
            let below = |l: &str| l.starts_with(&prefix) && l[prefix.len()..].contains('/');
            remove(dbs, &below);
            let db = Db::new(path, rng);
            dbs.insert(lane.clone(), db);
            dbs.get_mut(&lane).unwrap().commit(cluster, rng);
            (format!("reset {lane}"), true)
        }
        28..38 if !prefix.is_empty() => {
            let parent = prefix.strip_suffix('/').unwrap();
            let parent = parent.rsplit_once('/').map_or("", |(p, _)| p);
            let parent = if parent.is_empty() {
                String::new()
            } else {
                format!("{parent}/")
            };
            let dst = parent + &facet_prefix([name]);
            if dst == prefix {
                return (format!("clone {prefix} onto itself"), false);
            }
            remove(dbs, &|l| l.starts_with(&dst));
            let copies: Vec<_> = (dbs.iter())
                .filter_map(|(l, db)| {
                    Some((format!("{dst}{}", l.strip_prefix(&prefix)?), db.state()))
                })
                .collect();
            for (lane, state) in copies {
                let path = files.place(&lane).unwrap();
                dbs.insert(lane, Db::open(path, state));
            }
            cluster
                .clone_lanes(NS, ACTOR, &prefix, &dst, files)
                .unwrap();
            (format!("clone {prefix} {dst}"), true)
        }
        38..44 => {
            dbs.get_mut(&lane).unwrap().checkpoint();
            (format!("checkpoint {lane}"), false)
        }
        // A facet closed, and started again under the running root.
        44..48 if !prefix.is_empty() => {
            let db = dbs.get_mut(&lane).unwrap();
            db.checkpoint();
            let path = db.path.display().to_string();
            cluster.open_lane(NS, ACTOR, &path, &lane).unwrap();
            (format!("restart {lane}"), false)
        }
        _ => {
            dbs.get_mut(&lane).unwrap().commit(cluster, rng);
            (format!("commit {lane}"), true)
        }
    }
}

fn run(seed: u64, steps: usize) -> Result<(), String> {
    let mut sim = Sim::new(seed);
    for _ in 0..steps {
        sim.step();
        if !sim.world.borrow().violations.is_empty() {
            break;
        }
    }
    let settled = sim.world.borrow().violations.is_empty() && sim.settle();
    let world = sim.world.borrow();
    let _ = std::fs::remove_dir_all(&sim.root);
    if settled && world.violations.is_empty() {
        return Ok(());
    }
    let tail = world.trace.len().saturating_sub(80);
    Err(format!(
        "{}\n{}",
        if world.violations.is_empty() {
            "never taken over once faults stopped".into()
        } else {
            world.violations.join("\n")
        },
        world.trace[tail..].join("\n")
    ))
}

#[test]
fn every_takeover_inherits_what_was_acknowledged() {
    crate::dst::seeds(50, |seed| run(seed, 1500));
}
