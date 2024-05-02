# Changelog

## v1.16.0-pre.2

Summary of Changes
------------------

**Major Changes:**
* Add Kubernetes EndpointSlice synchronization from Cilium clustermesh (#28440, @MrFreezeex)
* iptables: Add rules runtime reconciliation (#31372, @pippolo84)
* k8s: Add support for Kubernetes 1.30.0 (#31687, @christarazi)
* Support CEL expressions in hubble flow filters (#31070, @chancez)

**Minor Changes:**
* "cilium-dbg map get ..." can now be called on BPF maps without cache (#31620, @AwesomePatrol)
* Add clustermesh hostname endpointslice synchronization (#31814, @MrFreezeex)
* Add option to automatically discover k8sServiceHost and k8sServicePort info (kubeadm clusters only) (#31885, @kreeuwijk)
* Add option to disable ExternalIP mitigation (CVE-2020-8554). (#31513, @kvaster)
* Add support for deploying clustermesh-apiserver with multiple replicas for high availability. (#31677, @thorn3r)
* Added source pod metadata to generated L7 DNS visibility policies. (#32166, @nebril)
* Adds `IPv6Pool` field to the spec of CiliumNodes CRD to list of IPv6 addresses available to the node for allocation.
* Adds `service_implementation_delay` metric accounting the duration in seconds to propagate the data plane programming of a service, its network and endpoints from the time the service or the service pod was changed excluding the event queue latency (#32055, @ovidiutirla)
* bpf: WireGuard: detect tunnel traffic in native-routing mode (#31586, @julianwiedmann)
* Configure restrictive security contexts by default for clustermesh-apiserver containers (#31540, @giorio94)
* daemon: Do not require NodePort for WireGuard (#32249, @brb)
* datapath: Move WG skb mark check to to-netdev (#31751, @brb)
* egressgw: remove deprecated install-egress-gateway-routes option (#32105, @julianwiedmann)
* envoy: Bump envoy image for golang 1.22.2 (#31774, @sayboras)
* envoy: Bump envoy minor version to v1.29.x (#31571, @sayboras)
* envoy: Bump envoy version to v1.28.2 (#31810, @sayboras)
* envoy: Update envoy 1.29.x to v1.29.4 (#32137, @sayboras)
* Expose clustermesh-apiserver version through a dedicated command, and as part of logs (#32165, @giorio94)
* Feat add nodePort.addresses value to set nodeport-addresses in the cilium configmap (#31672, @eyenx)
* Fix LRP error cases where node-local redirection was erroneously skipped. Extend LRP spec in order for users to explicitly skip node-local redirection from LRP selected backend pods. (#26144, @aditighag)
* Forcefully terminate stale sockets in the host netns connected to deleted LRP backends when socket-lb is enabled, and allow applications to re-connect to active LRP backends. (#32074, @aditighag)
* gateway-api: appProtocol support (GEP-1911) (#31310, @rauanmayemir)
* gateway-api: Sync up with upstream (#31806, @sayboras)
* helm: Cleanup old k8s version check and deprecated atributes (#31940, @sayboras)
* Helm: possibility to install operator as standalone app (#32019, @balous)
* helm: Remove deprecated option containerRuntime.integration (#31942, @sayboras)
* hubble/correlation: Support deny policies (#31544, @gandro)
* Hubble: add possibility to export flows to container logs (#31422, @siegmund-heiss-ich)
* hubble: add trace reason support in hubble flows (#31226, @kaworu)
* hubble: support drop\_reason\_desc in flow filter (#32135, @chaunceyjiang)
* install/kubernetes: add extraInitContainers (#32245, @bewing)
* ipset: Rework the reconciler to use batch ops (#31638, @pippolo84)
* labels: Add controller-uid into default ignore list (#31964, @sayboras)
* loader: attach programs using tcx (#30103, @rgo3)
* Make endpointslice clustermesh syncing opt-out for headless services (#32021, @MrFreezeex)
* Skip overlay traffic in the BPF SNAT processing, and thus reduce pressure on the BPF Connection tracking and NAT maps. (#31082, @julianwiedmann)
* StateDB based Health (#30925, @tommyp1ckles)
* Support configuring TLS for hubble metrics server (#31973, @chancez)
* WireGuard: Deprecate userspace fallback (#31867, @gandro)

**Bugfixes:**
* Agent: add kubeconfigPath to initContainers (#32008, @darox)
* Avoid drops with "CT: Unknown L4 protocol" for non-ICMP/TCP/UDP traffic, caused by an error check in the BPF NAT engine. (#31820, @julianwiedmann)
* daemon: Run conntrack GC after Endpoint Restore (#32012, @joestringer)
* dnsproxy: Fix bug where DNS request timed out too soon (#31999, @gandro)
* Envoy upstream connections are now unique for each downstream connection when using the original source address of a source pod. (#32270, @jrajahalme)
* envoy: pass idle timeout configuration option to cilium configmap (#32203, @mhofstetter)
* Fix azure ipam flake caused by instance resync race condition. (#31580, @tommyp1ckles)
* Fix bpf_sock compilation for ipv6-only (#30553, @alexferenets)
* Fix failing service connections, when the service requests are transported via cilium's overlay network. (#32116, @julianwiedmann)
* Fix incorrect reporting of the number of etcd lock leases in cilium-dbg status. (#31781, @giorio94)
* Fix issue causing clustermesh-apiserver/kvstoremesh to not start when run with a non-root user (#31539, @giorio94)
* Fix service connection to terminating backend, when the service has no more backends available. (#31840, @julianwiedmann)
* Fix synchronization of CiliumEndpointSlices when running the Cilium Operator in identity-based slicing mode. (#32239, @thorn3r)
* Fixed a race condition in service updates for L7 LB. (#31744, @jrajahalme)
* Fixes a bug where Cilium in chained mode removed the `agent-not-ready` taint too early if the primary network is slow in deploying. (#32168, @squeed)
* Fixes a route installing issue which may cause troubles for cilium downgrade. (#31716, @jschwinger233)
* Fixes an (unlikely) bug where HostFirewall policies may miss updates to a node's labels. (#30548, @squeed)
* fqdn: fix memory leak in transparent mode when there was a moderately high number of parallel DNS requests (>100). (#31959, @marseel)
* fqdn: Fix minor restore bug that causes false negative checks against a restored DNS IP map. (#31784, @nathanjsweet)
* Ingress/Gateway API: merge Envoy listeners for HTTP(S) and TLS passthrough (#31646, @mhofstetter)
* ingress: Set the default value for max_stream_timeout (#31514, @tskinn)
* Introduce fromEgressProxyRule (#31923, @jschwinger233)
* ipam: retry netlink.LinkList call when setting up ENI devices (#32099, @jasonaliyetti)
* loader: sanitize bpffs directory strings for netdevs (#32090, @rgo3)
* Only read the relevant parts of secrets for originatingTLS (ca.crt) and terminatingTLS (tls.crt, tls.key) blocks in Cilium L7 policies. Fixes a bug where a ca.crt key in a secret passed to terminatingTLS incorrectly configures Envoy to require a client certificate on TLS connections from pods. Previous behavior can be restored with the --use-full-tls-context=true agent flag. (#31903, @JamesLaverack)

**CI Changes:**
* .github: Add workflow telemetry (#32037, @joestringer)
* .github: Pretty-print gateway API test results (#32039, @joestringer)
* alibabacloud/eni: avoid racing node mgr in test (#31877, @bimmlerd)
* ariane: Fix detection of changes to nat46x64 tests (#32070, @joestringer)
* ci-e2e-upgrade: Disable ingress-controller and bpf.tproxy=true (#31917, @brb)
* ci-e2e-upgrade: Make it stable (#31895, @brb)
* ci-l4lb: Remove unnecessary untrusted checkout (#32071, @joestringer)
* ci: Add matrix for bpf.tproxy and ingress-controller (#31875, @sayboras)
* ci: Filter supported versions of AKS (#32303, @marseel)
* ci: Fix typo on "Ginkgo" (#32317, @qmonnet)
* ci: Increase timeout for images for l4lb test (#32201, @marseel)
* ci: only install llvm/clang and gingko for gingko test suite changes (#32309, @tklauser)
* ci: remove build artifacts in integration tests to prevent space issues (#32050, @giorio94)
* ci: run privileged unit tests only once (#31779, @tklauser)
* ci: Set hubble.relay.retryTimeout=5s (#32066, @chancez)
* ci: use base and head SHAs from context in lint-build-commits workflow (#32140, @tklauser)
* CODEOWNERS: Remove the catch-all rule (#32174, @michi-covalent)
* Don't cache LLVM in the CI to resolve disk space issues. (#32045, @gentoo-root)
* enable kube cache mutation detector (#32069, @aanm)
* Fix ipset reconciler unit tests (#31836, @pippolo84)
* fix k8s versions tested in CI (#31966, @nbusseneau)
* Fix node throughput (#31825, @marseel)
* Fix sysctl reconciler unit tests (#31833, @pippolo84)
* gha: configure fully-qualified DNS names as external targets (#31510, @giorio94)
* gha: drop double installation of Cilium CLI in conformance-eks (#32042, @giorio94)
* Miscellaneous improvements to the clustermesh upgrade/downgrade test (#31958, @giorio94)
* Modify GitHub Actions Workflows to echo the inputs they are given when triggered by a `workflow_dispatch` event. (#31424, @learnitall)
* Move cilium/hubble code to cilium/cilium repo (#31893, @michi-covalent)
* Remove ariane scheduled workflows for 1.12 (#32126, @marseel)
* Revert "test: Disable hostfw in monitor aggregation test" (#32315, @qmonnet)
* Scrape pprofs in 100 node scale test workflow for extra debugging information (#32056, @learnitall)
* Simplify NAT46x64,recorder tests (#32068, @joestringer)
* Spread ariane-scheduled workflows over multiple hours (#32142, @marseel)
* Test endpoint slice synchronization as part of the Conformance Cluster Mesh workflow (#31551, @giorio94)
* Test IPsec + KPR (#31760, @pchaigno)
* test/helpers: Skip CiliumUninstall if not installed (#32272, @joestringer)
* test: De-flake xds server_e2e_test (#32004, @jrajahalme)
* test: Remove redundant IPsec test (#31759, @pchaigno)
* test: remove unused assertion helpers (#32157, @tklauser)
* Use Clang from cilium-builder image to build BPF code in CI (#31754, @gentoo-root)
* workflows: Bump the timeout for Ginkgo tests (#31991, @pchaigno)
* workflows: Fix CI jobs for push events on private forks (#32085, @pchaigno)
* workflows: Remove stale CodeQL workflow (#32084, @pchaigno)

**Misc Changes:**
* Accurately manage the teardown sequence of an Endpoint's BPF resources (#32167, @ti-mo)
* Add Pod eviction warning in upgrade notes for Envoy DS (#31971, @learnitall)
* Add Spectro Cloud to USERS.md (#32027, @kreeuwijk)
* Add Syself to USERS.md (#32204, @lucasrattz)
* agent: Replace gocheck with built-in go test (#32214, @sayboras)
* bgpv1: check services for reconciliation if iTP=local (#31963, @harsimran-pabla)
* bgpv2: introducing service reconciler in BGPv2 reconcilers (#31962, @harsimran-pabla)
* BGPv2: Updates CiliumBGPNodeConfigOverride Type (#31598, @danehans)
* bitlpm: Document and Fix Descendants Bug (#31851, @nathanjsweet)
* bpf/test: Adjust mock function to reflect changes in tail_ipvX_policy (#31738, @jschwinger233)
* bpf: Add BPF map operations for the StateDB reconciler (#32123, @joamaki)
* bpf: add multicast in MAX_OVERLAY_OPTIONS (#32129, @harsimran-pabla)
* bpf: ct: clean up redundant 0-initializiations for CT entry creation (#31788, @julianwiedmann)
* bpf: hide dynamic/static variant for policy tail-call (#32299, @julianwiedmann)
* bpf: host: restore HostFW for overlay traffic in to-netdev (#31818, @julianwiedmann)
* bpf: lb: remove extra SVC lookup when backend lookup fails (#31595, @julianwiedmann)
* bpf: minor tail-call cleanups (#31990, @julianwiedmann)
* bpf: nodeport: avoid revalidation in nodeport_rev_dnat_ingress_ipv4() (#32044, @julianwiedmann)
* bpf: nodeport: split off LB logic in nodeport_lb*() (#31590, @julianwiedmann)
* bpf: tests: don't define HAVE_ENCAP in IPsec tests (#31737, @julianwiedmann)
* bpf: update `set_ipsec_encrypt` to optionally fill SPI with node map value (#31804, @ldelossa)
* bugtool: Dump raw node ID map (#31741, @pchaigno)
* build(deps): bump github.com/docker/docker from 26.0.1+incompatible to 26.0.2+incompatible (#32072, @dependabot[bot])
* build(deps): bump idna from 3.4 to 3.7 in /Documentation (#31916, @dependabot[bot])
* build(deps): bump pydantic from 2.3.0 to 2.4.0 in /Documentation (#32176, @dependabot[bot])
* build: golangci-lint: update go version configuration (#32191, @mhofstetter)
* chore(deps): update all github action dependencies (main) (#31951, @renovate[bot])
* chore(deps): update all github action dependencies (main) (#31992, @renovate[bot])
* chore(deps): update all github action dependencies (main) (#32101, @renovate[bot])
* chore(deps): update all github action dependencies (main) (#32237, @renovate[bot])
* chore(deps): update all-dependencies (main) (#31694, @renovate[bot])
* chore(deps): update all-dependencies (main) (#32242, @renovate[bot])
* chore(deps): update cilium/cilium-cli action to v0.16.6 (main) (#32219, @renovate[bot])
* chore(deps): update docker.io/library/golang:1.22.2 docker digest to 450e382 (main) (#31949, @renovate[bot])
* chore(deps): update docker.io/library/golang:1.22.2 docker digest to d5302d4 (main) (#32218, @renovate[bot])
* chore(deps): update docker/setup-buildx-action action to v3.3.0 (main) (#31832, @renovate[bot])
* chore(deps): update gcr.io/distroless/static-debian11:nonroot docker digest to f41b84c (main) (#31815, @renovate[bot])
* chore(deps): update gcr.io/distroless/static-debian11:nonroot docker digest to f41b84c (main) (#31950, @renovate[bot])
* chore(deps): update github/codeql-action action to v3.24.10 (main) (#31816, @renovate[bot])
* chore(deps): update go to v1.22.2 (main) (#31767, @renovate[bot])
* chore(deps): update hubble cli to v0.13.3 (main) (#32102, @renovate[bot])
* chore(deps): update kylemayes/install-llvm-action action to v2.0.1 (main) (#31746, @renovate[bot])
* CI: bump default FQDN datapath timeout from 100 to 250ms (#31866, @squeed)
* cilium-dbg: avoid leaking file resources (#31750, @tklauser)
* cilium-dbg: Expose Cilium network routing status (#32036, @joestringer)
* cilium-dbg: fix exported command name (#31606, @lmb)
* cilium-health: Fix setting of disable_ipv6 sysctl (#32120, @joamaki)
* cli: Replace gocheck with built-in go test (#32210, @sayboras)
* cloud-provider: Replace gocheck with built-in go test (#32212, @sayboras)
* clustermesh: fix panic if the etcd client cannot be created (#32225, @giorio94)
* cmd, watchers: Populate ipcache in case of high-scale ipcache (#31848, @pchaigno)
* cni: Improve logging with common fields (#31805, @sayboras)
* contexthelpers: remove unused package (#31834, @tklauser)
* controller: Remove unused function FakeManager() (#32011, @joestringer)
* datapath/iptables: remove unused customChain.feederArgs (#31876, @tklauser)
* datapath: report distinct drop reason for missed endpoint policy tailcall (#32151, @julianwiedmann)
* Deactivated Grafana reporting in monitoring example yaml. (#31989, @mvtab)
* dev: Clean-up development setup (#32277, @sayboras)
* docs: Add annotation for Ingress endpoint (#32284, @sayboras)
* docs: Add connectivity perf test introduction as a part of e2e tests. (#31731, @fujitatomoya)
* docs: add EnableDefaultDeny documentation (#32097, @squeed)
* docs: Add table for which pkts are encrypted with WG (#31557, @brb)
* docs: clean up example yaml for L4 Deny Policy (#32015, @huntergregory)
* docs: Correct name of "cert-manager" in tab groups (#31929, @JamesLaverack)
* docs: Document build framework for docs (#32006, @qmonnet)
* docs: Fix pep-8 style for conf.py (#32009, @joestringer)
* docs: Fix prometheus port regex (#32030, @JBodkin-Amphora)
* Docs: improve Flatcar section (#31986, @darox)
* docs: Improve CiliumEndpointSlice documentation to prepare graduation to "Stable" (#31800, @antonipp)
* docs: Make ICMP rules for the Host Firewall easier to read/search (#31900, @qmonnet)
* Docs: mark Tetragon as Stable (#31886, @sharlns)
* docs: Update LLVM requirement to LLVM 17 (#32236, @pchaigno)
* Document Cluster Mesh global services limitations when KPR=false (#31798, @giorio94)
* Don't expand CIDR labels, match smartly in Labels instead (#30897, @squeed)
* Drop unused service-related test helpers (#32002, @giorio94)
* egressgw: minor bpf refactors (#32094, @julianwiedmann)
* egressgw: Miscellaneous minor fixes to the manager (#31869, @pippolo84)
* egressgw: reject config with EnableIPv4Masquerade false (#32150, @ysksuzuki)
* endpoint / ApplyPolicyMapChanges: fix incorrect comment (#31790, @squeed)
* endpoint: clean up unused code (#32081, @tklauser)
* endpoint: Skip build queue warning log is context is canceled (#32132, @jrajahalme)
* endpoint: skip Envoy incremental updates if no Envoy redirects (#31454, @squeed)
* endpoint: skip Envoy incremental updates if no Envoy redirects (try 2) (#31775, @squeed)
* endpoint: store state in ep_config.json (#31559, @lmb)
* envoy: add support to bind to privileged ports (#32158, @mhofstetter)
* Fix helm chart incompatible types for comparison (#32025, @lou-lan)
* Fix spelling in DNS-based proxy info (#31728, @saintdle)
* fix(deps): update all go dependencies main (main) (#31578, @renovate[bot])
* fix(deps): update all go dependencies main (main) (#31853, @renovate[bot])
* fix(deps): update all go dependencies main (main) (#31952, @renovate[bot])
* fix(deps): update all go dependencies main (main) (#32106, @renovate[bot])
* fix(deps): update all go dependencies main (main) (#32222, @renovate[bot])
* fix(deps): update all go dependencies main (main) (#32256, @renovate[bot])
* fix: close verifier.log (#32018, @testwill)
* fix: deduplicate ConfigMap key if ENI mode and endpointRoutes are enabled (#31891, @remi-gelinas)
* Fixes redundant space on the introduction page (intro.rst) (#32206, @network-charles)
* gh/actions: Bump CLI to v0.16.6 (#32271, @brb)
* golangci: Enable errorlint (#31458, @jrajahalme)
* helm: no operator hostPorts when hostNetwork is disabled (#32127, @balous)
* hive: Rebase on cilium/hive (#32020, @bimmlerd)
* hive: Reduce hive trace logs to debug level (#32033, @joestringer)
* hubble: Support --cel-expression filter in hubble observe (#32147, @chancez)
* images: Update bpftool, checkpatch images (#31753, @qmonnet)
* images: Update LLVM to 17.0.6 (#31418, @gentoo-root)
* Improve compatibility with LLVM 17. (#31849, @gentoo-root)
* Improve dev-doctor version detection and error reporting (#32035, @joestringer)
* Improve release organization page (#31970, @joestringer)
* ingress: change hostnetwork default port to unprivileged 8080 (#32159, @mhofstetter)
* ingress: move flag `ingress-default-xff-num-trusted-hops` to cell config (#32190, @mhofstetter)
* ingress: remove json struct tags from internal ingress translation model (#31659, @mhofstetter)
* install/kubernetes: add AppArmor profile to Cilium Daemonset (#32199, @aanm)
* install/kubernetes: update nodeinit image to latest version (#32181, @tklauser)
* ipcache: Replace gocheck with built-in go test (#32283, @sayboras)
* ipsec: Debug info for transient IPsec upgrade drops (#32240, @pchaigno)
* k8s: Replace gocheck with built-in go test (#32211, @sayboras)
* kvstore: always use scoped logger to distinguish different client instances (#32087, @giorio94)
* l2announcer: Use the device table to access devices (#31931, @joamaki)
* l7 policy: add possibility to configure Envoy proxy xff-num-trusted-hops (#32200, @mhofstetter)
* lb: Replace gocheck with built-in go test (#32282, @sayboras)
* Loader reconciliation preparatory changes (#31773, @dylandreimerink)
* loader: remove CompileAndLoad (#31792, @lmb)
* loader: rewrite tests to remove gocheck dependency (#31841, @lmb)
* Makefile: Run generate-k8s-api in builder image (#32063, @joestringer)
* Misc BGP Control Plane documents (#31670, @YutaroHayakawa)
* Move governance docs to the Cilium community repo (#31692, @katiestruthers)
* multicast: check support for batch lookup (#31892, @harsimran-pabla)
* operator: Replace gocheck with built-in go test (#32215, @sayboras)
* pkg/bgp: Replace gocheck with built-in go test (#32263, @sayboras)
* pkg/endpoint: Replace gocheck with built-in go test (#32262, @sayboras)
* pkg/envoy: Replace gocheck with built-in go test (#32280, @sayboras)
* pkg/ipam: Replace gocheck with built-in go test (#32227, @sayboras)
* pkg/metrics: Replace gocheck with built-in go test (#32226, @sayboras)
* policy/k8s: Fix bug where policy synchronization event was lost (#32028, @gandro)
* policy: Remove unused `allow-remotehost-ingress` derivedFrom label (#32058, @gandro)
* Prepare for release v1.16.0-pre.1 (#31733, @joestringer)
* Print verbose verifier logs on verifier errors in socketlb (#31321, @gentoo-root)
* README: Update releases (#31734, @joestringer)
* Readme: Updates for release 1.15.4, 1.14.10, 1.13.15 (#32098, @asauber)
* Refactor InitK8sSubsystem and adding unit tests (#31645, @anubhabMajumdar)
* Remove aks-preview from AKS workflows (#32118, @marseel)
* Remove CiliumOperatorName constant (#31597, @miono)
* Remove hostPort dependency on BPF NodePort (#32046, @chaunceyjiang)
* Remove Hubble-OTel from the roadmap (#31847, @xmulligan)
* Remove superfluous nolint comments (#31743, @tklauser)
* Remove v1.12 from Container Vulnerability Scan (#32114, @marseel)
* Replace `option.Config.{Get,Set,Append}Devices` by table lookups (#30578, @bimmlerd)
* Revert "Remove hostPort dependency on BPF NodePort" (#32160, @squeed)
* route: Also compare ip rule mask for lookupRule (#31700, @jschwinger233)
* Seamlessly downgrade bpf attachments from tcx to tc (#32228, @ti-mo)
* Transition to NodeMapV2 which now includes SPI in its map values. (#31431, @ldelossa)
* update cilium/certgen to v0.1.11 (#31863, @rolinh)
* Update module health report for cilium status CLI (#30429, @derailed)
* Update USERS.md - add Gcore info on supporting Cilium (#31763, @rzdebskiy)
* WireGuard: remove cleanup for obsolete IP rules (#31874, @julianwiedmann)

**Other Changes:**
* cli: make multicast subscriber list exportable (#31799, @harsimran-pabla)