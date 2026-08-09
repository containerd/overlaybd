# Why Virtual Machines Should Use Overlaybd Images

Every virtual machine boots from a virtual disk, and the format of
that disk decides how fast the VM starts, how much host memory its
bookkeeping costs, how cheaply it can be snapshotted, and how easily
it can be distributed. The incumbent VM image formats — qcow2,
VHD/VHDX, and VMDK — were designed for an era of single hypervisor
hosts, shallow snapshot chains, and monolithic images copied around
as whole files. Cloud infrastructure breaks all three assumptions.
This article argues that [overlaybd](https://github.com/containerd/overlaybd)
— a new image format that features variable block size and an
efficient query algorithm — is a stronger foundation for virtual
machines.

## The Incumbent Formats

- **qcow2** (QEMU/KVM), the "QEMU copy-on-write" format v2, is the
  native disk format of the QEMU ecosystem and the default in most
  KVM-based virtualization stacks. It locates data through a two-level
  L1/L2 mapping table over fixed-size clusters (64 KB by default),
  with a separate reference-count table tracking which clusters are
  allocated. Internal snapshots, compression, and encryption were all
  added over the years, folded into the same file.

- **VHD / VHDX** (Microsoft Hyper-V, Azure). VHD descends from
  Connectix Virtual PC and became Microsoft's standard virtual-disk
  format after the 2003 acquisition; VHDX, introduced with Windows
  Server 2012, raised the capacity limit from 2 TB to 64 TB and added
  a metadata journal for crash resilience. Both locate data through a
  flat Block Allocation Table (BAT) over much larger blocks — 2 MB in
  VHD, and 32 MB by default in VHDX.

- **VMDK** (VMware) is not one format but a family: monolithic sparse
  disks, two-gigabyte split extents, stream-optimized files for OVA
  distribution, and ESXi's snapshot variants. All of them locate data
  through grain directories and grain tables — 64 KB grains in hosted
  products, 4 KB grains in ESXi's SESparse variant.

Different tables, same architecture: each image is a virtual block
device plus an allocation table that translates guest block addresses
into file offsets.

## External Chains: Snapshots That Fit the Image Model

Snapshots come in two forms:

- **Internal snapshots (qcow2 only).** qcow2 can keep every
  generation of data inside a single file. For one VM on one host
  this works well — the file is self-contained and easy to move, and
  QEMU users have relied on it for years. VHD and VMDK have no
  equivalent: Hyper-V checkpoints and VMware snapshots always create
  new files.

- **External chains (all three formats).** Each snapshot becomes a
  separate file that points at its parent — qcow2's backing file,
  VHD's differencing disk (via parent locators), VMDK's delta disk
  (via `parentFileNameHint` and the CID chain).

For a single VM, internal snapshots are the better tool: no parent
files to track, and no chain of files to walk on reads. A platform
makes the opposite trade. A cloud image service does not manage one
disk; it manages thousands of disks derived from a handful of base
images, and it wants to store the base once, share it across every
derivative, and transfer only the snapshots that changed — the
economics that container registries proved out.

Internal snapshots give none of that. With snapshots welded into one
file, there is no sharing a base between images, no pushing or pulling
a single snapshot, no deduplication across images — every derived
image carries a full private copy of everything.

External chains match the OCI image model exactly. An OCI image is a
stack of layers — one file per layer, each holding only the
incremental content added over its predecessor. In VM terms, a layer
is precisely a snapshot: the delta on top of a parent. Layers are
content-addressable, shared across images, and pulled incrementally.
A backing-file chain has the same shape: the base image is a shared
read-only file, and each VM adds a small private top snapshot.
Platforms that instead ship monolithic disks — for example wrapping a
whole qcow2 file inside a container image — forfeit all of this and
pay a full download on every change.

## The O(n) Lookup Problem

In a chain, a read that misses the top file falls through to its
parent, then the grandparent, until some snapshot holds the data: O(n)
in chain depth, plus a metadata lookup in every file along the way.

That long chains hurt is not our extrapolation — it is the vendors'
own operational guidance:

- **VMware** supports
  [at most 32 snapshots in a chain](https://knowledge.broadcom.com/external/article/318825/best-practices-for-using-vmware-snapshot.html),
  recommends "only 2 to 3 snapshots" for better performance, warns
  against retaining any snapshot for more than 72 hours, and provides
  snapshot consolidation precisely to collapse chains back down.

- **Microsoft** flags an
  ["excessive number of checkpoints (more than 50)"](https://learn.microsoft.com/en-us/troubleshoot/windows-server/virtualization/hyper-v-snapshots-checkpoints-differencing-disks)
  as a problem to fix by merging them away, and ships the merge
  tooling (`Merge-VHD`, Edit Disk) to collapse differencing-disk
  chains back down.

- **QEMU** provides `commit` and `rebase` in
  [qemu-img](https://www.qemu.org/docs/master/tools/qemu-img.html)
  for offline merging, and live `block-commit`/`block-stream` jobs
  for running guests, to fold chains back into fewer files; the
  [libvirt docs](https://libvirt.org/kbase/merging_disk_image_chains.html)
  call a long chain "long and cumbersome" and walk through reducing
  its length live, without guest downtime.

This guidance exists because chain reads get slower with every link.
Yet a layered image ecosystem wants the opposite: base OS, language
runtime, dependencies, application, per-tenant customization —
dozens of layers are normal. The distribution model demands deep
stacks; the format only tolerates shallow ones.

## Index Memory — Multiplied by Chain Depth

Keeping the mapping table hot is what makes random I/O fast, and
these tables are not small:

- **qcow2.** At the default 64 KB cluster size, the L1/L2 tables run
  roughly 12.5 MB per 100 GB of data — the tables are allocated
  lazily and track populated data, not virtual size. The 8-byte
  entries are wide because they carry more than an offset: flag bits
  mark each cluster as copied, zero, or compressed, and the offset
  field addresses an exabyte-scale file. QEMU sizes each file's L2
  cache to cover its whole table, up to 32 MB by default on Linux
  (raised from 1 MB in QEMU 3.1).

- **VMDK.** Four-byte grain-table entries over 64 KB grains come to
  roughly 6 MB per 100 GB — half of qcow2's figure at the same
  granularity. The saving is paid for in function: the entry is just
  a sector number (zero means unallocated), leaving no room for
  per-grain flags such as zero or compressed, and capping a single
  extent at 2 TB.

- **VHD/VHDX.** The BAT is tiny — about 200 KB per 100 GB in VHD,
  25 KB in VHDX — a saving bought with 2 MB/32 MB allocation blocks,
  whose cost shows up in the next section.

Crucially, these numbers are *per file*: every snapshot in the
chain adds its own table and its own cache to the hypervisor's
memory, so the footprint grows with chain depth. Sparse allocation
keeps an empty snapshot cheap, but the data-bearing snapshots —
the base image above all — are fully populated by definition. A
30-snapshot qcow2 chain thus carries not 12.5 MB but up to
30 × 12.5 MB of tables, plus a separate cache for every file.

And the spread between these numbers is no accident: each is set by
the format's granularity choice. Finer granularity means more table
entries and more memory; coarser granularity shrinks the table but
taxes every write to a shared block. Trading index memory against
copy-on-write cost is a dilemma common to all three formats — the
subject of the next section.

## The Dilemma: Index Memory Overhead vs Copy-on-Write Overhead

Every fixed-granularity table sits on the same trade-off curve. Table
size is proportional to virtual size divided by cluster size;
copy-on-write cost is proportional to cluster size. Choosing a
cluster size means choosing which of the two to pay:

- **Small clusters** keep writes cheap but inflate the table —
  qcow2's 64 KB default is what produces the 12.5 MB per 100 GB of
  data footprint above.

- **Large clusters** shrink the table but tax every write. qcow2 with
  2 MB clusters needs only ~400 KB of tables per 100 GB of data, but
  the first write to a shared cluster must preserve the rest of it — a
  4 KB write can trigger a multi-megabyte copy. VHD sits at this end
  of the curve outright: its 2 MB blocks make the BAT negligible and
  write amplification structural. VHDX softens this in differencing
  disks — a block can be marked partially present, with a sector
  bitmap choosing per sector between child and parent, so a
  partial-block write skips the copy-up — but pays with an extra
  metadata structure and a journaled bitmap update on every first
  write to a sector: the tax moves from copy-up to metadata I/O,
  still on the same curve. VMware
  went the other way, shrinking grains (64 KB, then 4 KB in
  SESparse) to limit amplification — and pays in metadata volume and
  consolidation complexity instead.

No fixed-granularity format escapes this curve; you can move along
it, but not off it.

## How Overlaybd Resolves All of It

Overlaybd was designed against exactly these constraints, replacing
the fixed-granularity table per file with one merged, extent-based
index per device. Point by point:

- **Variable-length blocks: small index, no copy-up.** LSMT records
  map extents — variable-length block ranges rather than fixed-size
  cluster slots — so index size tracks fragmentation, not virtual
  size: in Alibaba's production environment, images larger than
  50 GB average under 300 KB of merged index (16 bytes per record).
  And because the mapping is decoupled from write granularity, writes
  land at 512-byte sector granularity: a write always covers whole
  sectors, never triggers a copy-up, and costs in proportion to its
  own size.
  ![images indices alibaba production](assets/img-index.png ":size=80%")

- **O(1) lookup at any depth.** All snapshot indices merge into a
  single LSMT index when the image loads. Chain depth never enters
  the data path — a read resolves against one index whether the image
  carries two snapshots or fifty, so deep, OCI-style layering stops
  being a performance question.

- **A query engine built for speed.** The merged index is a
  linearized B+ tree whose lookups are vectorized with AVX-512,
  sustaining [hundreds of millions of queries per second](lsmt_lookup.md)
  for index resolution. Metadata lookup is never the bottleneck.

- **OCI-compatible layering.** Each snapshot ships as a
  separate file, so a base image is stored once and reused by every
  derivative VM — deduplicated in both storage and transfer. The
  image layout follows the OCI image spec, and the distribution path
  is compatible with OCI image registries and their ecosystem.

- **Optional fast compression.** Data can be stored compressed with
  LZ4 or zstd in small, independently addressable units (ZFile), so a
  random read fetches and decompresses only the relevant unit.
  Compression is optional per image, trading a little CPU for
  transfer and storage savings.

## Production Evidence

This is not a theoretical argument. Overlaybd is deployed at scale
today, across VMs, microVMs, and containers alike.

- **Alibaba** has run overlaybd for years across its entire
  application portfolio — Taobao, TMall, AlibabaCloud, and more — and
  commercialized it on AlibabaCloud as the container image
  acceleration offering. AlibabaCloud's Function Compute runs
  function microVMs on the same architecture. Two peer-reviewed
  papers document these deployments:

  - [DADI](https://www.usenix.org/conference/atc20/presentation/li-huiba)
    (ATC'20), describing overlaybd's large-scale deployment at
    Alibaba, and
  - [FaaSNet](https://www.usenix.org/conference/atc21/presentation/wang-ao)
    (ATC'21), applying it to Function Compute.

- **[Azure](https://learn.microsoft.com/en-us/azure/aks/artifact-streaming-overview)**
  built Azure Kubernetes Service's Artifact Streaming feature on
  overlaybd.

- **[Databricks](https://www.databricks.com/blog/booting-databricks-vms-7x-faster-serverless-compute)**
  reported 7× faster VM startup for serverless compute using
  overlaybd-based image acceleration on Azure.
  [Superhuman](https://www.databricks.com/blog/how-superhuman-and-databricks-built-200k-qps-inference-platform-together)
  built a 200K-QPS inference platform on the same infrastructure.

- **[DeepSeek Elastic Compute](https://arxiv.org/html/2606.19348v1)**
  runs its execution environment on overlaybd-format images.

- **[fly.io](https://community.fly.io/t/experimental-speedy-machine-creation-with-overlaybd/18958)**
  boots Firecracker microVMs backed by overlaybd images.

- **[Google Colab](https://medium.com/@gogasca_/using-overlaybd-to-improve-startup-time-6c5f90f23345)**
  starts a 27 GB runtime image on GCE in ~170 ms with a warm cache,
  and ~5.6 s cold.

- **[hocus.dev](https://hocus.dev/blog/virtualizing-development-environments)**
  uses overlaybd as the backing store for microVM-based development
  environments.

- **[Kimi AgentEnv](https://github.com/kvcache-ai/AgentEnv)**
  (Moonshot AI) re-implemented the overlaybd format as its sandbox
  image substrate, achieving
  [sub-second launch latency](https://github.com/MoonshotAI/Kimi-K3/blob/main/k3_tech_report.pdf).

## Conclusion

The incumbent VM disk formats share one architecture — a
fixed-granularity allocation table per file, chained one file per
snapshot — and inherit its three costs: reads that slow with chain
depth, index memory that multiplies with snapshots, and a write tax
set by cluster size. Overlaybd replaces the per-file tables with a
single merged extent index and takes chain depth out of the data
path, removing all three costs at once. For infrastructure
that starts, snapshots, and distributes virtual machines at scale, it
is the stronger foundation.

---

*Learn more:*
*[github.com/containerd/overlaybd](https://github.com/containerd/overlaybd) ·*
*[CNCF Slack #overlaybd](https://cloud-native.slack.com/archives/C07PCJHRZKY)*
