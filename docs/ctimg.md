# Why Containers and Secure Containers Should Use Overlaybd Images

Containers and secure containers are the backbone of modern cloud
workloads — from microservices and batch jobs to serverless functions
and VM-isolated "secure container" stacks like Kata Containers on
Firecracker. Their image infrastructure carries a demanding combination
of requirements: sub-second cold starts, efficiency at scale, rock-solid
stability with strong isolation, and a single format that spans ordinary
(runc) containers, secure containers, and non-Linux guests alike. No traditional image format
satisfies all of these at once. This article argues that
[overlaybd](https://github.com/containerd/overlaybd) — a layered,
lazily-loaded, seekably-compressed block-device image format — is the
best architectural fit for this workload.

## What Makes This Workload Different

A container platform creates and destroys instances at high frequency —
elastic scaling, short-lived batch jobs, and serverless functions — so
startup latency and efficiency at scale are first-class concerns even
though individual containers may also be long-lived. The workload
profile has four distinguishing characteristics:

1. **Cold start dominates.** Many containers are short-lived or scale
   from zero, and even long-lived ones restart on every deploy, scaling
   event, or failure. Image pulling is the single largest component of
   startup latency, so time to first useful work is governed by the
   image format.

2. **Efficiency at scale is decisive.** A single cluster may run
   thousands of containers simultaneously, all booting from shared base
   layers and competing for registry bandwidth, host memory, network
   throughput, and storage I/O. Images stack many shared layers, so
   per-access costs must not grow with layer depth. And for secure
   containers, every I/O additionally crosses a VM boundary.
   Per-operation overhead, per-instance memory, and per-container
   bandwidth are all magnified by scale.

3. **Stability and security are non-negotiable.** Containers have
   become the infrastructure of internet businesses: the image stack
   sits on the serving path of every workload, so it must be rock-solid
   — downtime is measured in lost revenue. And since these workloads
   are multi-tenant, the storage interface is part of the security
   perimeter as well.

4. **One format, multiple runtimes.** The same image should serve
   ordinary runc containers and VM-based secure containers — and
   ideally non-Linux guests such as Windows containers — without
   per-runtime variants or separate image pipelines.

Each of these, taken alone, can be addressed by existing technology.
Taken together, they rule out all conventional approaches.

## Why Filesystem-based Images Fall Short

Existing image stacks fall into two architectural camps:

- **Filesystem-based images.** The dominant OCI tar+gzip format is
  served through overlayfs; lazy-pull variants (eStargz, SOCI) defer
  extraction but keep tar and filesystem semantics; single-filesystem
  images (squashfs, EROFS) and FUSE-based formats (Nydus/RAFS) take the
  same file-level approach. Whenever the consumer is a VM — as in
  secure containers — the filesystem is typically mounted on the host
  and shared into the guest through virtio-fs or 9p, so the stack
  generally ends up depending on overlayfs-style stacking, a
  FUSE/virtio-fs serving path, or both.

- **Block-based images.** qcow2, the standard VM disk format, and
  overlaybd both present the image as a virtual block device instead.

The filesystem camp falls short on each of the four requirements:

**Cold start.** The standard OCI image is a stack of tar+gzip layers.
Starting a container requires downloading, decompressing, and
extracting every layer before the process can run — even though in
practice only a small fraction of image data is touched during
startup. For a multi-gigabyte image this means tens of seconds to
minutes of cold-start latency. P2P distribution tools (Dragonfly,
Kraken) speed up *distribution* across a cluster, but they cannot skip
the download-and-decompress steps themselves. Lazy-pull and
chunk-addressable variants do defer extraction, but remain bound to
archive structure and filesystem semantics — every first access still
fetches file-by-file through the serving stack, and in secure
containers that shared-filesystem path lands on the critical path as
well.

**Efficiency.** Whole-image pulls from thousands of simultaneously
starting containers saturate the registry and the network, and
traditional P2P only mirrors whole images between peers — every
container still fetches the entire image. File-level stacking resolves
each file access (open, stat, readdir, and so on) by searching every
layer's directory tree top-down — O(n) in the number of layers — and
read-only filesystem images must likewise stack through overlayfs,
inheriting the same walk. The first
modification to lower-layer data triggers a copy-up of the entire
file, a recurring cost proportional to file size, not write size. And
for secure containers, virtio-fs (or 9p) turns every lookup, getattr,
and open into a separate cross-VM FUSE round-trip, so a single open()
on a nested path may incur five or more round-trips: cheap operations
made expensive one round-trip at a time across a VM boundary.

**Stability and security.** Served into a VM or through
FUSE/virtio-fs, filesystem state lives in a userspace daemon; if it
dies, the mounted filesystem typically hangs or becomes unusable, and
recovery is difficult — a serious liability for infrastructure that
must serve internet businesses around the clock. Filesystem-level formats also
parse complex metadata (xattrs, ACLs, symlinks, device nodes) in host
or hypervisor context, exposing a large attack surface to untrusted
guest content — and virtio-fs/9p have even been shown to let guests
bypass memory-cgroup and ephemeral-storage accounting entirely,
because page population happens outside the sandbox
([Kata Containers #12203](https://github.com/kata-containers/kata-containers/issues/12203)).

**Runtime universality.** These formats are all Linux filesystem
machinery: a filesystem-level image assumes Linux-style semantics on
the consumer side, so none of them can serve a Windows container or
any non-Linux guest. Maintaining separate image variants and pipelines
for runc containers, secure containers, and Windows containers
multiplies build, test, and operational cost.

A block device sidesteps all of the above by construction: the
consumer runs its own filesystem on top, so metadata operations stay
local to the consumer, the serving path never has to parse filesystem
structures, and any OS that understands a block device can consume the
image. EROFS now supports native sub-filesystem merging to avoid the
O(n) layer walk, and can expose a merged view through one or more block
devices. This is an explicit acknowledgment that the problems above
are real and worth solving.

## Overlaybd vs qcow2

Both block-based options inherit these structural advantages, but the
differences between them are decisive. qcow2 carries three structural
costs.

The first is lookup cost. To conform to the layered-image model,
operators must chain separate qcow2 files via backing files, and a
read miss walks the chain downward — O(n) in the number of layers.
(Native internal snapshots keep all data in one file, but they do not
conform to the layered-image model.)

The second is the index. Data is located through a two-level L1/L2
mapping table that grows with the data stored — roughly 12.5 MB per
100 GB of data at the default 64 KB cluster size — and every file in
a backing chain carries its own table and its own cache, so the
footprint multiplies with chain depth. QEMU sizes each file's cache
to cover its whole table, up to 32 MB by default, leaving operators
stuck between two losses: keep the caches and spend host memory, or
shrink them and waste I/O on table entries that miss the cache.

The third is write overhead. Writes are copy-on-write at cluster
granularity: a write allocates a new cluster, copies the original
content over, and only then writes the new data — so the cost of a
write is the cluster copy plus the write itself.

Overlaybd avoids all three by design. It merges all layer indices
into a single compact index — under 300 KB on average for 50 GB+
images in Alibaba's production environment, small enough to keep
fully memory-resident — resolves any block in O(1) regardless of
layer count, and sends writes straight to the writable layer with no
data copying.

So of the two block-based formats, overlaybd wins on lookup cost,
index footprint, and write overhead.

## How Overlaybd Meets Each Need

Overlaybd presents each image as a virtual block device backed by a
stack of layers. The host serves data on demand; a VM consumer mounts
the device inside the guest, and a runc container sees a normal
filesystem — typically ext4 — mounted from the device on the host.

### Cold start: on-demand fetching

Containers and VMs start by reading image data remotely as needed,
without downloading or unpacking the whole image; a multi-gigabyte image
can launch in under a second. Data is compressed in small, independently
addressable units (ZFile), so a random read fetches and decompresses
only the relevant unit — and because the compressed transfer is smaller,
end-to-end latency is frequently lower than for uncompressed reads. For
workloads with known access patterns, trace-based or file-list-based
prefetch warms the local cache ahead of demand, hiding network latency.

### Efficiency

- **P2P on-demand loading.** Each container pulls only the blocks it
  actually reads, so aggregate download volume is a fraction of a
  whole-image pull. Those fetches are spread across peers with P2P
  distribution (dadi-p2proxy), relieving the registry precisely when
  thousands of containers start at once.

- **Compact, memory-resident index.** At image load time overlaybd
  merges all layer indices into a single LSMT index; in Alibaba's
  production environment, images larger than 50 GB have an average index
  under 300 KB. Thousands of concurrent instances keep their
  indices fully memory-resident with no eviction.

- **O(1) cross-layer lookup.** Because the layer indices are merged at
  load time, runtime block lookup is constant-time regardless of layer
  count — a linearized B+ tree with AVX-512 SIMD acceleration sustains
  [hundreds of millions of QPS](lsmt_lookup.md) for index resolution. And the index
  merging algorithm for block layers is also much simpler than that for
  file system layers.

- **Directly writable.** The writable layer is part of the format
  itself, so writes need no extra overlayfs upper stacked on top —
  unlike read-only image formats, which must add one before the first
  write. The I/O stack stays simpler and more efficient end to end.

- **No copy-on-write penalty.** The writable layer is indexed at
  sector (512-byte) granularity — the smallest unit of any block I/O
  a filesystem issues — so a write always covers whole sectors and
  never triggers a copy. Write cost is proportional to write size,
  not to file or cluster size.

- **No metadata round-trips.** For secure containers the image is
  attached as a standard block device (virtio-blk, with vhost-user-blk
  a future option): the guest runs its own filesystem internally, so
  all metadata operations complete in-guest with zero cross-VM
  round-trips. Only data I/O crosses the boundary, at block granularity,
  and the guest OS merges adjacent requests before submission.

### Stability and security

- **Stable by construction.** Overlaybd's state lives in a thin,
  well-defined interface and on-disk format, so the service can recover
  from process crashes or restarts and re-attach the device. The core
  logic is just an LBA-to-data mapping — far simpler than a filesystem
  implementation — and the kernel block layer adds request queuing,
  retries, and error handling for free.

- **Minimal attack surface, sound isolation.** The host sees only a
  sequence of block reads and writes. It never has to parse guest
  filesystem metadata, never interprets symlinks or xattrs, and page
  accounting stays inside the guest — eliminating the virtio-fs/9p
  accounting-bypass class of issue.

### Runtime universality: one format

A block device is the one storage interface every runtime and OS
supports. The same overlaybd format and OCI distribution pipeline
serve runc containers, VM-based secure containers, and
Windows containers (even on Linux hosts, or vice versa), with no
per-runtime variant. Image contents and filesystem choices remain
platform-specific — ext4, XFS, Btrfs, EROFS, or NTFS.

## Production Evidence

This is not a theoretical argument. Overlaybd is deployed at scale today.

### Containers at scale

- **Alibaba.** Born in Alibaba and deployed for years across its entire
  application portfolio — Taobao, TMall, AlibabaCloud, and more —
  overlaybd is the container image acceleration offering on
  AlibabaCloud, adopted by major customers worldwide.

- **[Azure](https://learn.microsoft.com/en-us/azure/aks/artifact-streaming-overview).**
  Azure Kubernetes Service's Artifact Streaming feature is built on
  overlaybd, accelerating container startup for AKS customers.

- **[Databricks](https://www.databricks.com/blog/booting-databricks-vms-7x-faster-serverless-compute).**
  Reported 7× VM startup improvement using overlaybd-based image
  acceleration on Azure.
  [Superhuman](https://www.databricks.com/blog/how-superhuman-and-databricks-built-200k-qps-inference-platform-together)
  also adopted Databricks' infrastructure to build a 200K QPS inference
  platform.

- [And more...](?id=who-uses-overlaybd)

### Peer-reviewed research

The design is documented in two USENIX Annual Technical Conference
papers, both about container workloads: the
[DADI system](https://www.usenix.org/conference/atc20/presentation/li-huiba)
(ATC'20), which describes overlaybd's large-scale container
infrastructure at Alibaba, and
[FaaSNet](https://www.usenix.org/conference/atc21/presentation/wang-ao)
(ATC'21), which applies it to AlibabaCloud's Function Compute service.

## Conclusion

How the mainstream image stacks compare on these four requirements:

| Image stack | Cold start | Efficiency at scale | Stability & security | Runtime universality |
| --- | --- | --- | --- | --- |
| Filesystem-based stacks | lazy variants exist, still bound to archive and fs semantics | whole-image pulls or per-operation FUSE/virtio-fs round-trips; O(n) layer walk; copy-up | userspace fs daemon on the FUSE/fscache/virtio-fs serving path; fs metadata parsed in the trust boundary; known [virtio-fs security vulnerabilities](https://github.com/kata-containers/kata-containers/issues/12203) | assumes a Linux fs stack |
| qcow2 | lazy load possible (qemu-nbd/HTTP) | O(n) backing chains; L2 cache trades memory against I/O; cluster CoW | block interface; no fs metadata parsing | VMs natively; runc via qemu-nbd |
| **overlaybd** | on-demand block fetch; sub-second start | P2P on-demand; O(1) lookup; small memory footprint; no copy-up | crash-recoverable; strong isolation with battle-tested interface | runc + secure containers (+ Windows); OCI-compatible |

Containers and secure containers need to start fast, stay efficient at
scale, run stably and securely as business-critical infrastructure, and
share a single image format across runtimes. Taken together, these
requirements point to one architectural choice: a layered, lazily-loaded,
seekably-compressed block-device image.

[Overlaybd](https://github.com/containerd/overlaybd) is an open-source
(Apache-2.0), CNCF-governed implementation of this design, battle-tested
in production at Alibaba, Azure, and Databricks. If you are building a
container or secure-container platform and have not yet committed to an
image format, it deserves a serious look.

---

*Learn more:*
*[github.com/containerd/overlaybd](https://github.com/containerd/overlaybd) ·*
*[CNCF Slack #overlaybd](https://cloud-native.slack.com/archives/C07PCJHRZKY)*
