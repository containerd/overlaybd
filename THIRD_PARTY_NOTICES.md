# Third-Party Notices

## tcmu-runner (via photon-libtcmu)

The sources under `src/libtcmu` were imported from
[data-accelerator/photon-libtcmu](https://github.com/data-accelerator/photon-libtcmu)
at commit `cdcbc3cae674136da05f1f043525e11e79845c77`.

photon-libtcmu is derived from
[open-iscsi/tcmu-runner](https://github.com/open-iscsi/tcmu-runner). It contains
only the libtcmu-related subset needed by consumers and does not include the
tcmu-runner daemon, handlers, or backing stores. The retained libtcmu code was
adapted to use PhotonLibOS and includes fixes that were not present in
tcmu-runner.

photon-libtcmu was originally maintained as a separate project so that it
could track and synchronize with tcmu-runner. Because tcmu-runner is no longer
actively maintained, continued synchronization is no longer practical.
Rather than develop photon-libtcmu as an independent fork, its maintained
source has therefore been incorporated into OverlayBD.

The principal upstream for the incorporated code is tcmu-runner. The original
copyright and license notices in the source files are retained. Those files
are offered under the recipient's choice of the GNU Lesser General Public
License, version 2.1 or any later version, or the Apache License, version 2.0.
OverlayBD elects to use and distribute this bundled copy under Apache-2.0. The
complete Apache-2.0 license text is preserved in:

- `src/libtcmu/LICENSE.Apache2`

Changes made for PhotonLibOS integration, additional fixes, and subsequent
OverlayBD maintenance are recorded in the corresponding project histories and
OverlayBD commits.
