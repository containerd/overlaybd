# Why Containers and Secure Containers Should Use Overlaybd Images

Containers and secure containers are the backbone of modern cloud
workloads — from microservices and batch jobs to serverless functions
and VM-isolated "secure container" stacks like Kata Containers on
Firecracker. Their image infrastructure carries a demanding combination
of requirements: sub-second cold starts, efficiency at scale, rock-solid
stability with strong isolation, and a single format that spans ordinary
(runc) and secure containers alike. No traditional image format
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
   per-access costs multiply with layer depth. And for secure
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
   ordinary runc containers and VM-based secure containers without
   per-runtime variants or separate image pipelines.

Each of these, taken alone, can be addressed by existing technology.
Taken together, they rule out all conventional approaches.

## Why Traditional Formats Fall Short

### Cold-start latency

The standard OCI image is a stack of tar+gzip layers. Starting a
container requires downloading, decompressing, and extracting every
layer before the process can run — even though in practice only a small
fraction of image data is touched during startup. For a multi-gigabyte
image this means tens of seconds to minutes of cold-start latency.

P2P distribution tools (Dragonfly, Kraken) speed up *distribution*
across a cluster, but they cannot skip the download-and-decompress
steps themselves, so they do little for startup latency. Lazy-pull
solutions (eStargz, SOCI) do defer extraction, but remain bound to
tar's structural limitations and filesystem semantics.

### Efficiency

Even where traditional formats are functionally usable, their efficiency
is poorly matched to container workloads, and scale amplifies every
inefficiency:

- **Registry and network saturation under concurrency.** Whole-image
  pulls from thousands of simultaneously starting containers saturate
  the registry and the network. Traditional P2P mirrors whole images
  between peers, which spreads the load but does not reduce the
  per-container download volume — every container still fetches the
  entire image.

- **Index memory at density (qcow2).** qcow2 — the natural block format
  for VM-backed secure containers — locates each 64 KB cluster through
  a two-level L1/L2 mapping table. For a 100 GB image this is ~12.5 MB
  of metadata that must live in host memory. At high density that
  footprint dominates: hundreds or thousands of concurrent VMs each
  holding a multi-megabyte table consume gigabytes of host memory in
  metadata alone — memory no longer available to the workloads
  themselves.

- **O(n) lookup cost with layer depth.** OCI layers are stacked with
  overlayfs, which resolves each file access (open, stat, readdir, and
  so on) by searching every layer's directory tree top-down — O(n) in
  the number of layers. EROFS is a single-layer read-only filesystem,
  so a multi-layer image built from EROFS must stack mounts with
  overlayfs and inherits the same O(n) walk. qcow2's native internal
  snapshots avoid this (all data lives in one file), but to conform to
  the layered-image model operators must chain separate qcow2 files via
  backing files, where a read miss walks the chain downward — again
  O(n). Containers accumulate many layers, so every random read pays an
  ever-growing penalty.

- **Copy-up on first write.** The first modification to data residing
  in a lower layer triggers a full copy before the write can proceed:
  overlayfs copies the entire file to the upper layer; EROFS, being
  read-only, places writes in an overlayfs upper layer and incurs the
  same file-level copy-up; qcow2 allocates a new cluster and copies the
  original content. Containers frequently modify image contents at
  runtime, so this is a recurring cost proportional to file/cluster
  size, not write size.

- **Metadata round-trip multiplier (virtio-fs).** The common choice for
  secure containers, virtio-fs (or 9p), shares a host filesystem with
  the guest: every lookup, getattr, and open becomes a separate cross-VM
  FUSE round-trip, so a single open() on a nested path may incur five
  or more round-trips. The operations themselves are cheap; the cost is
  doing them one-at-a-time across a VM boundary instead of in-batch
  locally.

### Stability and security

Filesystem-based stacks fall short on the two properties that
matter most for business-critical infrastructure:

- **FUSE-based stacks are fragile.** virtio-fs (virtiofsd) and Nydus's
  nydusd keep filesystem state in a userspace daemon; if it dies, the
  mounted filesystem typically hangs or becomes unusable, and recovery
  is difficult. For infrastructure that must serve internet businesses
  around the clock, a storage stack with this failure mode is a serious
  liability.

- **Filesystem metadata is parsed inside the trust boundary.**
  Filesystem-level formats parse complex metadata (xattrs, ACLs,
  symlinks, device nodes) in host or hypervisor context, exposing a
  large attack surface to untrusted guest content. virtio-fs and 9p
  have even been shown to let guests bypass memory-cgroup and
  ephemeral-storage accounting entirely, because page population happens
  outside the sandbox
  ([Kata Containers #12203](https://github.com/kata-containers/kata-containers/issues/12203)).

### Runtime universality

Filesystem-level formats tie you to a specific runtime and OS. overlayfs
and EROFS are Linux kernel features; Nydus's RAFS format requires its
nydusd userspace daemon exposed over FUSE or virtiofs. All of them
assume a Linux-style filesystem stack: none can serve a Windows
container or any non-Linux guest. Maintaining separate image variants
and pipelines for runc containers, secure containers, and Windows
containers multiplies build, test, and operational cost.

## How Overlaybd Meets Each Need

Overlaybd presents each image as a virtual block device backed by a
stack of layers. The host serves data on demand; the consumer — VM or
container alike — sees a mountable block device carrying a normal
filesystem, typically ext4.

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
  under 300 KB — roughly 40× smaller than qcow2's ~12.5 MB L1/L2 table
  for a comparable image. Thousands of concurrent instances keep their
  indices fully memory-resident with no eviction.

- **O(1) cross-layer lookup.** Because the layer indices are merged at
  load time, runtime block lookup is constant-time regardless of layer
  count — a linearized B+ tree with AVX-512 SIMD acceleration sustains
  over 100 million QPS for metadata resolution.

- **No copy-on-write penalty.** A write goes directly to the writable
  layer, the lower-layer block is left untouched, and the LSMT index
  simply records the new location. Write cost is proportional to write
  size, not to file or cluster size.

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
  sequence of block reads and writes. It never parses guest filesystem
  metadata, never interprets symlinks or xattrs, and page accounting
  stays inside the guest — eliminating the virtio-fs/9p
  accounting-bypass class of issue.

### Runtime universality: one format

A block device is the one storage interface every runtime and OS
supports. The same overlaybd image serves runc containers (via a
containerd snapshotter over TCMU or ublk), VM-based secure containers
(via virtio-blk), and potentially Windows containers, with no
per-runtime variant. The user is free to run any filesystem on top —
ext4, XFS, Btrfs, EROFS, or NTFS. The format is OCI-compatible and
works with existing registries, so adoption does not require a new
distribution pipeline.

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
| OCI tar+gzip (overlayfs) | ✗ full download and unpack | ✗ whole-image pulls; O(n) layer walk; copy-up | ~ kernel-stable; host kernel parses fs metadata | ✗ Linux only |
| eStargz / SOCI | ~ lazy pull, still tar-bound | ✗ overlayfs stacking and copy-up | ~ FUSE snapshotter in the serving path | ✗ Linux only |
| qcow2 | ~ lazy load possible (qemu-nbd/HTTP) | ✗ multi-MB index per VM; O(n) backing chains; cluster CoW | ✓ block interface; no fs metadata parsing | ~ runc via qemu-nbd and VMs, but no standard OCI tooling |
| EROFS | ✗ full image required | ✗ read-only and single-layer; stacks via overlayfs | ~ kernel-stable; host kernel parses fs metadata | ✗ Linux only |
| Nydus (RAFS) | ✓ lazy chunk loading | ~ FUSE/virtio-fs round-trips per operation | ✗ userspace daemon (nydusd); known security vulnerabilities over virtio-fs ([Kata Containers #12203](https://github.com/kata-containers/kata-containers/issues/12203)) | ✗ assumes a Linux fs stack |
| **overlaybd** | ✓ on-demand block fetch; sub-second start | ✓ P2P on-demand; O(1) lookup; small memory footprint; no copy-up | ✓ crash-recoverable; strong isolation with battle-tested interface | ✓ runc + secure containers (+ Windows); OCI-compatible |

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
